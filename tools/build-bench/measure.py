#!/usr/bin/env python3
"""RealmMesh 构建测量工具。

按场景驱动 CMake/CTest，经 launcher.cpp 记录每次真实编译与链接，每秒采样整机内存与测量进程树 RSS，
结果写入 --out 目录；summarize 子命令按场景汇总中位数、极差与噪声带。用法见同目录 README.md。
"""

import argparse
import contextlib
import hashlib
import json
import os
import platform
import re
import shutil
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path

EVENTS_ENV = 'REALMMESH_BUILD_BENCH_EVENTS'
SOURCE_SUFFIXES = ('.c', '.cc', '.cpp', '.cxx')
NOISE_FLOOR = 0.05

# 代表探针：主指标向文件末尾追加改变预处理 token、不改行为的声明（variant_bytes 的 token 变体），
# 历史对照仍追加注释（comment 变体）；测完恢复原始字节。
PROBES = {
    'lua-cpp': 'framework/scripting/src/lua_runtime.cpp',
    'lua-hpp': 'framework/scripting/include/realmmesh/scripting/lua_runtime.hpp',
    'gateway-hpp': 'game/gateway/include/realmmesh/game/gateway/gateway_runtime.hpp',
    'player-data-hpp': 'game/common/include/realmmesh/game/common/player_data_store.hpp',
    'proto': 'proto/realmmesh/common/v1/envelope.proto',
}
SCENARIOS = ['fetch', 'cold-entry', 'repro', 'hot-full', 'unit-entry', 'cpp-entry', 'probes']
# libsodium 是 ExternalProject：下载包放进其默认 DOWNLOAD_DIR 且 hash 相符时，构建跳过下载。
SODIUM_CMAKE = 'third_party/sodium/CMakeLists.txt'
SODIUM_DOWNLOAD_DIR = 'third_party/sodium/sodium_external-prefix/src'
SODIUM_CACHE = 'sodium-download'
TIMELINE_MARKERS = ('Building ', 'Linking ', 'Performing ', 'Downloading', 'Test #', 'Test  #', 'tests passed', 'QUIC transport')
QUIC_ENABLED = 'realm_network: QUIC transport enabled'
QUIC_DISABLED = 'realm_network: QUIC transport disabled'


# ---------------------------------------------------------------- 纯函数（test_measure.py 覆盖）

def _resolve(path, cwd):
    candidate = Path(path)
    if not candidate.is_absolute() and cwd:
        candidate = Path(cwd) / candidate
    return os.path.normpath(str(candidate))


def classify_event(event, source_root):
    """给 launcher 事件补 kind/source/output/dependency。

    依赖编译 = 源码位于 FetchContent 的 _deps 下或源码根之外（--deps-dir 复用的依赖源码）；
    构建树里生成的源码（如 .pb.cc）算项目编译。
    """
    argv = event['argv']
    cwd = event.get('cwd') or None
    out = dict(event)
    is_compile = '-c' in argv
    out['kind'] = 'compile' if is_compile else 'link'
    source = None
    if is_compile:
        after_c = argv.index('-c') + 1
        if after_c < len(argv) and argv[after_c].endswith(SOURCE_SUFFIXES):
            source = argv[after_c]
        else:
            source = next((x for x in argv[1:] if x.endswith(SOURCE_SUFFIXES) and not x.startswith('-')), None)
    if source:
        source = _resolve(source, cwd)
    out['source'] = source
    output = None
    if '-o' in argv:
        index = argv.index('-o') + 1
        if index < len(argv):
            output = _resolve(argv[index], cwd)
    out['output'] = output
    root = os.path.normpath(str(source_root))
    inside = bool(source) and (source == root or source.startswith(root + os.sep))
    out['dependency'] = bool(source) and ('/_deps/' in source or not inside)
    return out


def sample_stats(values):
    """中位数、最小–最大值与噪声带 max(极差÷中位数, 5%)；单样本没有噪声带。"""
    ordered = sorted(float(v) for v in values)
    median = statistics.median(ordered)
    noise = None
    if len(ordered) >= 2:
        spread = (ordered[-1] - ordered[0]) / median if median else 0.0
        noise = max(spread, NOISE_FLOOR)
    return {'n': len(ordered), 'median': median, 'min': ordered[0], 'max': ordered[-1], 'noise': noise,
            'values': [float(v) for v in values]}


def stage_group(name):
    """去掉末尾样本序号：probe-lua-hpp-2 → probe-lua-hpp。"""
    return re.sub(r'-\d+$', '', name)


def delay_to_next_second(now):
    """距下一个整秒再留 50 ms 的等待时长。

    GNU Make 3.81（macOS /usr/bin/make）按整秒比较 mtime：与上次构建产物同一秒内写入的改动
    不会触发重编译。改写源码前等到 now 之后的整秒，保证改动的秒数严格晚于已有产物。
    """
    return int(now) + 1.05 - now


def variant_bytes(original, relative, label, index, kind):
    """探针第 index 次的改写内容。

    comment：末尾追加注释，供历史对照（预处理后不变）。
    token：末尾追加改变预处理 token、不改行为的声明——C++ 为全局 inline constexpr 变量
    （外部链接，不触发未使用告警），proto 为空 message；不同 index 内容不同。
    """
    if kind == 'comment':
        return original + f'\n// build-bench {label} content-change sample {index}\n'.encode()
    if kind != 'token':
        raise ValueError(f'unknown variant kind {kind}')
    if relative.endswith('.proto'):
        return original + f'\nmessage BuildBenchVariant{index} {{}}\n'.encode()
    if relative.endswith(('.cpp', '.hpp', '.h', '.cc')):
        name = 'realmmesh_build_bench_' + re.sub(r'\W', '_', label) + '_variant'
        return original + f'\ninline constexpr int {name} = {index};\n'.encode()
    raise ValueError(f'no token variant for {relative}')


@contextlib.contextmanager
def edited(path, write):
    """只读取一次原始字节；with 块内的各样本都由它派生变体，退出（含异常）时用 write 恢复原始字节。"""
    original = path.read_bytes()
    try:
        yield original
    finally:
        write(path, original)


def parse_ps(text):
    """`ps -A -o pid=,ppid=,rss=` 输出 → [(pid, ppid, rss_kib)]。"""
    rows = []
    for line in text.splitlines():
        parts = line.split()
        if len(parts) == 3 and all(p.isdigit() for p in parts):
            rows.append(tuple(int(p) for p in parts))
    return rows


def tree_rss_kib(rows, root):
    """root 及其全部后代进程的 RSS 合计（KiB）。"""
    children, rss = {}, {}
    for pid, ppid, kib in rows:
        children.setdefault(ppid, []).append(pid)
        rss[pid] = kib
    if root not in rss:
        return 0
    total, stack = 0, [root]
    while stack:
        pid = stack.pop()
        total += rss.get(pid, 0)
        stack.extend(children.get(pid, []))
    return total


def effective_available_mib(mem_available_mib, cgroup_max_bytes, cgroup_current_bytes):
    """Linux 实际可用内存：宿主 MemAvailable 与 cgroup 限额余量取小。"""
    if cgroup_max_bytes is None:
        return mem_available_mib
    return min(mem_available_mib, (cgroup_max_bytes - (cgroup_current_bytes or 0)) / 1048576)


def parse_external_url(text):
    """ExternalProject 的 URL 与 SHA256（小写）。"""
    url = re.search(r'^\s*URL\s+(\S+)\s*$', text, re.M)
    digest = re.search(r'^\s*URL_HASH\s+SHA256=([0-9A-Fa-f]+)\s*$', text, re.M)
    if not url or not digest:
        raise ValueError('URL / URL_HASH SHA256 not found')
    return url.group(1), digest.group(1).lower()


_CTEST_SUMMARY = re.compile(r'(\d+)% tests passed, (\d+) tests? failed out of (\d+)')
_CTEST_FAILED = re.compile(r'Test\s+#\d+: (\S+) .*\*\*\*(?:Failed|Timeout|Exception|Not Run)')
_CTEST_REAL = re.compile(r'Total Test time \(real\) =\s*([\d.]+) sec')


def parse_ctest_log(text):
    summary = _CTEST_SUMMARY.search(text)
    if not summary:
        return {}
    failed = []
    for match in _CTEST_FAILED.finditer(text):
        if match.group(1) not in failed:
            failed.append(match.group(1))
    real = _CTEST_REAL.search(text)
    return {
        'tests_total': int(summary.group(3)),
        'tests_failed': int(summary.group(2)),
        'failed_tests': failed,
        'ctest_real_s': float(real.group(1)) if real else None,
    }


def summarize(rows):
    """按场景分组统计；预热样本不计入。总墙钟取每次整体实测，各步另列，不相加。"""
    groups = {}
    for row in rows:
        if not row.get('warmup'):
            groups.setdefault(stage_group(row['name']), []).append(row)
    result = []
    for group, members in groups.items():
        failures = []
        for row in members:
            if row.get('exit', 0) != 0:
                text = f"{row['name']}: exit {row['exit']}"
                if row.get('failed_tests'):
                    text += '; failed ' + ', '.join(row['failed_tests'])
                failures.append(text)
        entry = {
            'group': group,
            'wall_s': sample_stats([r['wall_s'] for r in members]),
            'compile_count': sample_stats([r.get('compile_count', 0) for r in members]),
            'link_count': sample_stats([r.get('link_count', 0) for r in members]),
            'dependency_compile_count': sample_stats([r.get('dependency_compile_count', 0) for r in members]),
            'failures': failures,
        }
        labels = []
        for row in members:
            for step in row.get('steps', []):
                if step['label'] not in labels:
                    labels.append(step['label'])
        if labels:
            entry['steps'] = [{'label': label, 'wall_s': sample_stats(
                [sum(s['wall_s'] for s in r.get('steps', []) if s['label'] == label) for r in members])}
                for label in labels]
        memory = [r['memory'] for r in members if r.get('memory')]
        if memory:
            available = [m['min_available_mib'] for m in memory if m.get('min_available_mib') is not None]
            pressure = [m['max_pressure_level'] for m in memory if m.get('max_pressure_level') is not None]
            tree = [m['max_tree_rss_mib'] for m in memory if m.get('max_tree_rss_mib') is not None]
            entry['memory'] = {
                'max_tree_rss_mib': max(tree) if tree else None,
                'min_available_mib': min(available) if available else None,
                'max_swap_growth_mib': max(m.get('swap_growth_mib') or 0 for m in memory),
                'max_pressure_level': max(pressure) if pressure else None,
                'oom_kills': sum(m.get('oom_kills') or 0 for m in memory),
            }
        tests = [r for r in members if 'tests_total' in r]
        if tests:
            entry['tests'] = [{'name': r['name'], 'total': r['tests_total'], 'failed': r['tests_failed']} for r in tests]
        result.append(entry)
    return result


def _fmt_seconds(stats):
    if stats['n'] == 1:
        return f"{stats['median']:.2f}"
    return f"{stats['median']:.2f}（{stats['min']:.2f}–{stats['max']:.2f}）"


def _fmt_count(stats):
    if stats['min'] == stats['max']:
        return f"{stats['median']:g}"
    return f"{stats['median']:g}（{stats['min']:g}–{stats['max']:g}）"


def render_markdown(summary):
    lines = [
        '| 场景 | n | 总墙钟秒，中位数（最小–最大） | 噪声带 | 编译 | 其中依赖 | 链接 | 进程树 RSS 峰值 MiB | 最低可用内存 MiB | swap 增长 MiB | 失败 |',
        '| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |',
    ]

    def mib(value):
        return f'{value:.0f}' if value is not None else '—'

    for g in summary:
        noise = g['wall_s']['noise']
        memory = g.get('memory') or {}
        lines.append('| {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |'.format(
            g['group'], g['wall_s']['n'], _fmt_seconds(g['wall_s']),
            f'{noise:.1%}' if noise is not None else '—',
            _fmt_count(g['compile_count']), _fmt_count(g['dependency_compile_count']), _fmt_count(g['link_count']),
            mib(memory.get('max_tree_rss_mib')), mib(memory.get('min_available_mib')),
            mib(memory.get('max_swap_growth_mib')),
            '<br>'.join(g['failures']) or '—',
        ))
    steps = [g for g in summary if g.get('steps')]
    if steps:
        lines += ['', '| 场景 | 步骤 | 墙钟秒，中位数（最小–最大） |', '| --- | --- | ---: |']
        for g in steps:
            for step in g['steps']:
                lines.append(f"| {g['group']} | {step['label']} | {_fmt_seconds(step['wall_s'])} |")
    return '\n'.join(lines) + '\n'


# ---------------------------------------------------------------- 内存采样

def _darwin_memory():
    text = subprocess.run(['vm_stat'], capture_output=True, text=True).stdout
    page = int(re.search(r'page size of (\d+) bytes', text).group(1))
    pages = {k: int(v) for k, v in re.findall(r'Pages (\w+):\s+(\d+)\.', text)}
    available = sum(pages.get(k, 0) for k in ('free', 'inactive', 'speculative', 'purgeable')) * page
    swap = subprocess.run(['sysctl', '-n', 'vm.swapusage'], capture_output=True, text=True).stdout
    used = re.search(r'used = ([\d.]+)([MG])', swap)
    swap_mib = float(used.group(1)) * (1024 if used.group(2) == 'G' else 1) if used else None
    level = subprocess.run(['sysctl', '-n', 'kern.memorystatus_vm_pressure_level'], capture_output=True, text=True).stdout.strip()
    return {'available_mib': available / 1048576, 'swap_used_mib': swap_mib,
            'pressure_level': int(level) if level.isdigit() else None, 'oom_kill_total': None}


def linux_cgroup_limit():
    """本进程 cgroup v2 祖先链中最紧的 memory.max：(目录, 字节)；无限额返回 (None, None)。"""
    try:
        line = next(l for l in Path('/proc/self/cgroup').read_text().splitlines() if l.startswith('0::'))
    except (OSError, StopIteration):
        return None, None
    best = (None, None)
    directory = Path('/sys/fs/cgroup') / line[3:].lstrip('/')
    while True:
        try:
            value = (directory / 'memory.max').read_text().strip()
            if value.isdigit() and (best[1] is None or int(value) < best[1]):
                best = (directory, int(value))
        except OSError:
            pass
        if directory == Path('/sys/fs/cgroup'):
            return best
        directory = directory.parent


def _linux_memory(cgroup):
    info = {}
    for line in Path('/proc/meminfo').read_text().splitlines():
        key, value = line.split(':', 1)
        info[key] = int(value.split()[0])
    oom = None
    for line in Path('/proc/vmstat').read_text().splitlines():
        if line.startswith('oom_kill '):
            oom = int(line.split()[1])
    directory, limit = cgroup
    current = int((directory / 'memory.current').read_text()) if directory else None
    return {'available_mib': effective_available_mib(info['MemAvailable'] / 1024, limit, current),
            'swap_used_mib': (info['SwapTotal'] - info['SwapFree']) / 1024,
            'pressure_level': None, 'oom_kill_total': oom}


def read_memory(cgroup=(None, None)):
    try:
        if sys.platform == 'darwin':
            return _darwin_memory()
        if sys.platform.startswith('linux'):
            return _linux_memory(cgroup)
    except (OSError, ValueError, AttributeError):
        pass
    return None


def read_tree_rss_mib(root):
    try:
        text = subprocess.run(['ps', '-A', '-o', 'pid=,ppid=,rss='], capture_output=True, text=True).stdout
    except OSError:
        return None
    return tree_rss_kib(parse_ps(text), root) / 1024


class MemorySampler(threading.Thread):
    """每秒采样测量进程树 RSS 合计、整机可用内存（Linux 计入 cgroup 限额）、swap、
    macOS 内存压力等级（1 正常/2 警告/4 严重）与 Linux OOM 计数。"""

    def __init__(self, path, cgroup):
        super().__init__(daemon=True)
        self.path = path
        self.cgroup = cgroup
        self.samples = []
        self._stop_event = threading.Event()
        self._start = time.perf_counter()

    def _sample(self):
        reading = read_memory(self.cgroup)
        if reading is None:
            return
        reading['tree_rss_mib'] = read_tree_rss_mib(os.getpid())
        reading['elapsed_s'] = time.perf_counter() - self._start
        self.samples.append(reading)
        with self.path.open('a') as stream:
            stream.write(json.dumps(reading) + '\n')

    def run(self):
        self._sample()
        while not self._stop_event.wait(1.0):
            self._sample()

    def stop(self):
        self._stop_event.set()
        self.join()
        self._sample()
        return self.summary()

    def summary(self):
        if not self.samples:
            return None
        swaps = [s['swap_used_mib'] for s in self.samples if s['swap_used_mib'] is not None]
        levels = [s['pressure_level'] for s in self.samples if s['pressure_level'] is not None]
        ooms = [s['oom_kill_total'] for s in self.samples if s['oom_kill_total'] is not None]
        trees = [s['tree_rss_mib'] for s in self.samples if s.get('tree_rss_mib') is not None]
        return {
            'samples': len(self.samples),
            'max_tree_rss_mib': max(trees) if trees else None,
            'min_available_mib': min(s['available_mib'] for s in self.samples),
            'swap_used_start_mib': swaps[0] if swaps else None,
            'swap_used_max_mib': max(swaps) if swaps else None,
            'swap_growth_mib': (max(swaps) - swaps[0]) if swaps else None,
            'max_pressure_level': max(levels) if levels else None,
            'oom_kills': (ooms[-1] - ooms[0]) if ooms else None,
        }


# ---------------------------------------------------------------- 场景驱动

class Bench:
    def __init__(self, args):
        self.args = args
        self.source = Path(args.source).resolve()
        self.out = Path(args.out).resolve()
        self.out.mkdir(parents=True, exist_ok=True)
        self.launcher = str(Path(args.launcher).resolve())
        self.build_dir = (self.source / (args.build_dir or f'build/{args.preset}')).resolve()
        self.deps_dir = Path(args.deps_dir).resolve() if args.deps_dir else self.out / 'deps-src'
        self.stages_path = self.out / 'stages.json'
        self.rows = json.loads(self.stages_path.read_text()) if self.stages_path.exists() else []
        self.env = os.environ.copy()
        for name in ['CMAKE_BUILD_PARALLEL_LEVEL', 'CTEST_PARALLEL_LEVEL', 'MAKEFLAGS', 'MFLAGS', 'MAKELEVEL',
                     'CMAKE_C_COMPILER_LAUNCHER', 'CMAKE_CXX_COMPILER_LAUNCHER']:
            self.env.pop(name, None)
        for name in list(self.env):
            # 测试夹具只用隔离 mongod；不把共享库连接信息带进测量进程。
            if 'MONGODB' in name and ('URI' in name or 'PASSWORD' in name):
                self.env.pop(name, None)
        self.env['LC_ALL'] = 'C'
        for item in args.env:
            key, _, value = item.partition('=')
            self.env[key] = value
        self.cgroup = linux_cgroup_limit() if sys.platform.startswith('linux') else (None, None)
        self.sodium_url, self.sodium_sha256 = parse_external_url((self.source / SODIUM_CMAKE).read_text())

    # 命令 --------------------------------------------------------------

    def deps_overrides(self):
        if not self.deps_dir.is_dir():
            return []
        return [f'-DFETCHCONTENT_SOURCE_DIR_{p.name[:-len("-src")].upper()}={p}'
                for p in sorted(self.deps_dir.iterdir()) if p.is_dir() and p.name.endswith('-src')]

    def configure_cmd(self, reuse_deps=True):
        launchers = [f'-DCMAKE_{lang}_{kind}_LAUNCHER={self.launcher}' for lang in ('C', 'CXX') for kind in ('COMPILER', 'LINKER')]
        return ['cmake', '--preset', self.args.preset] + launchers + (self.deps_overrides() if reuse_deps else []) + self.args.cmake_arg

    def build_cmd(self):
        return ['cmake', '--build', '--preset', self.args.preset] + (['--parallel', str(self.args.jobs)] if self.args.jobs else [])

    def ctest_cmd(self, *extra):
        return ['ctest', '--preset', self.args.preset] + list(extra)

    # 阶段 --------------------------------------------------------------

    def save(self):
        self.stages_path.write_text(json.dumps(self.rows, indent=2, ensure_ascii=False))

    def run(self, name, steps, warmup=False, extra=None):
        """steps 为 [(步骤名, argv)]，依次执行、遇失败即停。总墙钟整体实测，各步另记。"""
        name = self.args.name_prefix + name
        events = self.out / (name + '-events')
        if events.exists() and any(events.iterdir()):
            raise SystemExit(f'stage {name} already has events in {events}; use a new --out or --name-prefix')
        events.mkdir(exist_ok=True)
        env = dict(self.env, **{EVENTS_ENV: str(events)})
        log = self.out / (name + '.log')
        timeline = self.out / (name + '.timeline.jsonl')
        memory_path = self.out / (name + '.memory.jsonl')
        memory_path.unlink(missing_ok=True)
        sampler = MemorySampler(memory_path, self.cgroup)
        print('START ' + name, flush=True)
        loadavg_start = os.getloadavg()
        start = time.perf_counter()
        sampler.start()
        result = 0
        step_rows = []
        with log.open('w', buffering=1) as stream, timeline.open('w', buffering=1) as marks:
            for label, command in steps:
                stream.write('$ ' + ' '.join(command) + '\n')
                step_start = time.perf_counter()
                proc = subprocess.Popen(command, cwd=self.source, env=env, stdout=subprocess.PIPE,
                                        stderr=subprocess.STDOUT, text=True, errors='replace')
                for line in proc.stdout:
                    stream.write(line)
                    if any(marker in line for marker in TIMELINE_MARKERS):
                        marks.write(json.dumps({'elapsed_s': time.perf_counter() - start, 'line': line.rstrip()}) + '\n')
                result = proc.wait()
                step_rows.append({'label': label, 'command': command, 'wall_s': time.perf_counter() - step_start, 'exit': result})
                if result:
                    break
        elapsed = time.perf_counter() - start
        memory = sampler.stop()
        compiled, linked = [], []
        for path in events.glob('*.json'):
            event = classify_event(json.loads(path.read_text()), self.source)
            (compiled if event['kind'] == 'compile' else linked).append(event)
        compiled.sort(key=lambda e: e['wall_s'], reverse=True)

        def relative(path):
            if path and path.startswith(str(self.source) + os.sep):
                return os.path.relpath(path, self.source)
            return path

        row = {
            'name': name, 'warmup': warmup, 'steps': step_rows, 'wall_s': elapsed, 'exit': result,
            'loadavg_start': loadavg_start, 'loadavg_end': os.getloadavg(),
            'compile_count': len(compiled), 'link_count': len(linked),
            'dependency_compile_count': sum(1 for e in compiled if e['dependency']),
            'project_compile_count': sum(1 for e in compiled if not e['dependency']),
            'compile_wall_sum_s': sum(e['wall_s'] for e in compiled),
            'dependency_compile_wall_sum_s': sum(e['wall_s'] for e in compiled if e['dependency']),
            'project_compile_wall_sum_s': sum(e['wall_s'] for e in compiled if not e['dependency']),
            'link_wall_sum_s': sum(e['wall_s'] for e in linked),
            'max_compile_rss_kib': max((e.get('maxrss_kib', 0) for e in compiled), default=0),
            'max_link_rss_kib': max((e.get('maxrss_kib', 0) for e in linked), default=0),
            'failed_tool_calls': sum(1 for e in compiled + linked if e['exit']),
            'memory': memory,
            'log': log.name, 'events': events.name,
            'compiled_sources': [relative(e['source']) for e in compiled],
            'linked_outputs': sorted(relative(e['output']) or '?' for e in linked),
        }
        row.update(extra or {})
        row.update(parse_ctest_log(log.read_text(errors='replace')))
        self.rows.append(row)
        self.save()
        print('DONE ' + json.dumps({k: row[k] for k in ('name', 'wall_s', 'exit', 'compile_count', 'link_count')}), flush=True)
        return row

    @staticmethod
    def require(row, *labels):
        """labels 中的步骤失败即中止；测试步骤失败只记录，由 summarize 报出。"""
        for step in row['steps']:
            if step['label'] in labels and step['exit']:
                raise SystemExit(f"{row['name']}: {step['label']} failed with exit {step['exit']}")

    def fresh_build_dir(self):
        if self.build_dir.exists():
            shutil.rmtree(self.build_dir)

    def generated_version_headers(self):
        """构建树里生成的版本头（依赖源码目录除外）的 sha256 与 mtime。"""
        fingerprints = {}
        if not self.build_dir.exists():
            return fingerprints
        for directory, subdirs, files in os.walk(self.build_dir):
            subdirs[:] = [d for d in subdirs if not d.endswith('-src') and d != 'CMakeFiles']
            for file in files:
                if 'version' in file.lower() and file.endswith(('.h', '.hpp')):
                    path = Path(directory) / file
                    fingerprints[str(path.relative_to(self.source))] = {
                        'sha256': hashlib.sha256(path.read_bytes()).hexdigest(), 'mtime_ns': path.stat().st_mtime_ns}
        return fingerprints

    @staticmethod
    def write_source(path, content):
        """改写被测源码前等到下一个整秒，见 delay_to_next_second。"""
        time.sleep(delay_to_next_second(time.time()))
        path.write_bytes(content)

    def apply_variant(self, relative, original, label, index, kind):
        """由原始字节派生并写入探针变体；返回记录用的原/变体 hash。"""
        content = variant_bytes(original, relative, label, index, kind)
        self.write_source(self.source / relative, content)
        return {'variant': {
            'path': relative, 'kind': kind, 'index': index,
            'original_sha256': hashlib.sha256(original).hexdigest(),
            'variant_sha256': hashlib.sha256(content).hexdigest()}}

    def sample_indexes(self, count, warmup):
        """(阶段后缀, 变体序号, 是否预热)；预热序号 0，正式样本 1..count。"""
        return ([('warmup', 0, True)] if warmup else []) + [(str(i), i, False) for i in range(1, count + 1)]

    # 场景 --------------------------------------------------------------

    def scenario_fetch(self):
        """首次获取（独立场景，不并入已备源码冷构建）：新目录 FetchContent 全新获取并配置，
        保存依赖源码到 --deps-dir；再下载 libsodium 包并核对 SHA256，供冷入口预置。"""
        self.fresh_build_dir()
        row = self.run('fetch-configure', [('configure', self.configure_cmd(reuse_deps=False))])
        self.require(row, 'configure')
        if self.deps_dir.exists():
            shutil.rmtree(self.deps_dir)
        self.deps_dir.mkdir(parents=True)
        for src in sorted((self.build_dir / '_deps').glob('*-src')):
            shutil.copytree(src, self.deps_dir / src.name, symlinks=True)
        tarball = self.sodium_tarball()
        tarball.parent.mkdir(parents=True, exist_ok=True)
        row = self.run('fetch-sodium', [('download', ['curl', '-fsSL', '--retry', '3', '-o', str(tarball), self.sodium_url])])
        self.require(row, 'download')
        digest = hashlib.sha256(tarball.read_bytes()).hexdigest()
        if digest != self.sodium_sha256:
            raise SystemExit(f'libsodium sha256 {digest} != {self.sodium_sha256}')

    def sodium_tarball(self):
        return self.deps_dir / SODIUM_CACHE / self.sodium_url.rsplit('/', 1)[-1]

    def prepare_step(self):
        """把已核对的 libsodium 包放进 ExternalProject 下载目录，构建时 hash 相符即跳过下载。"""
        tarball = self.sodium_tarball()
        if not self.deps_overrides() or not tarball.is_file():
            raise SystemExit(f'no prepared sources in {self.deps_dir}; run the fetch scenario first or pass --deps-dir')
        return ('prepare', ['cmake', '-E', 'copy', str(tarball), str(self.build_dir / SODIUM_DOWNLOAD_DIR / tarball.name)])

    def scenario_cold_entry(self):
        """完整冷入口：产物为空、来源已备齐（含 libsodium 包）、无编译缓存：来源准备 + 配置 + 全量构建 + 完整 CTest。
        build 步即“已备齐固定源码的全量构建”。最后一次的构建目录留给 repro。"""
        for i in range(1, self.args.long_samples + 1):
            self.fresh_build_dir()
            row = self.run(f'cold-entry-{i}', [self.prepare_step(), ('configure', self.configure_cmd()),
                                               ('build', self.build_cmd()), ('test', self.ctest_cmd())])
            self.require(row, 'prepare', 'configure', 'build')

    def scenario_repro(self):
        """冷构建后同目录连续配置并构建 3 轮以上：首次重新配置单独报告，之后编译应为 0、生成版本头不变。"""
        before = self.generated_version_headers()
        for i in range(1, max(3, self.args.long_samples) + 1):
            for name, label, command in ((f'repro-configure-{i}', 'configure', self.configure_cmd()),
                                         (f'repro-build-{i}', 'build', self.build_cmd())):
                row = self.run(name, [(label, command)])
                after = self.generated_version_headers()
                row['generated_version_headers'] = len(after)
                row['generated_version_headers_changed'] = sorted(
                    p for p in set(before) | set(after) if before.get(p) != after.get(p))
                self.save()
                before = after

    def scenario_hot_full(self):
        """热完整验证：标准配置 + 稳定无操作 ALL 构建 + 完整 CTest（串行）。"""
        for i in range(1, self.args.long_samples + 1):
            self.run(f'hot-full-{i}', [('configure', self.configure_cmd()), ('build', self.build_cmd()),
                                       ('test', self.ctest_cmd())])

    def scenario_unit_entry(self):
        """默认 Unit、无改动：标准配置 + ALL 构建 + 全部 Unit（串行），整体计时。"""
        for suffix, _, warmup in self.sample_indexes(self.args.samples, warmup=True):
            self.run(f'unit-entry-{suffix}', [('configure', self.configure_cmd()), ('build', self.build_cmd()),
                                              ('test', self.ctest_cmd('-L', 'unit'))], warmup=warmup)

    def scenario_cpp_entry(self):
        """默认 Unit、代表 .cpp 真实改动：token 变体改 lua_runtime.cpp 后配置 + ALL 构建 + 全部 Unit。"""
        relative = PROBES['lua-cpp']
        with edited(self.source / relative, self.write_source) as original:
            for suffix, index, warmup in self.sample_indexes(self.args.samples, warmup=True):
                extra = self.apply_variant(relative, original, 'cpp-entry', index, 'token')
                row = self.run(f'cpp-entry-{suffix}', [('configure', self.configure_cmd()), ('build', self.build_cmd()),
                                                       ('test', self.ctest_cmd('-L', 'unit'))], warmup=warmup, extra=extra)
                if row['exit']:
                    break
        self.run('cpp-entry-restore', [('configure', self.configure_cmd()), ('build', self.build_cmd())])

    def scenario_probes(self):
        """每个探针：token 变体预热 1 次 + --samples 次（主指标），注释变体 --comment-samples 次（历史对照），
        之后恢复原始字节并构建回稳定状态。只计 build。"""
        for label in self.args.probe or list(PROBES):
            relative = PROBES[label]
            plan = [(f'probe-{label}-{suffix}', index, warmup, 'token')
                    for suffix, index, warmup in self.sample_indexes(self.args.samples, warmup=True)]
            plan += [(f'probe-{label}-comment-{i}', i, False, 'comment') for i in range(1, self.args.comment_samples + 1)]
            with edited(self.source / relative, self.write_source) as original:
                for name, index, warmup, kind in plan:
                    extra = self.apply_variant(relative, original, label, index, kind)
                    if self.run(name, [('build', self.build_cmd())], warmup=warmup, extra=extra)['exit']:
                        break
            self.run(f'probe-{label}-restore', [('build', self.build_cmd())])

    # 环境 --------------------------------------------------------------

    def environment(self, scenarios):
        def first_line(command):
            try:
                result = subprocess.run(command, capture_output=True, text=True, env=self.env, timeout=30)
                return (result.stdout or result.stderr).strip().splitlines()[0]
            except (OSError, IndexError, subprocess.TimeoutExpired):
                return None

        cache = {}
        cache_path = self.build_dir / 'CMakeCache.txt'
        if cache_path.exists():
            for line in cache_path.read_text(errors='replace').splitlines():
                match = re.match(r'^([A-Za-z_][\w.-]*):[A-Z]+=(.*)$', line)
                if match and match.group(1) in ('CMAKE_GENERATOR', 'CMAKE_BUILD_TYPE', 'CMAKE_C_COMPILER', 'CMAKE_CXX_COMPILER',
                                                'CMAKE_CXX_COMPILER_LAUNCHER', 'OPENSSL_INCLUDE_DIR', 'MSQUIC_LIBRARY', 'WITH_PROTOC'):
                    cache[match.group(1)] = match.group(2)
        quic = None
        for log in self.out.glob('*configure*.log'):
            text = log.read_text(errors='replace')
            if QUIC_ENABLED in text:
                quic = True
            elif QUIC_DISABLED in text and quic is None:
                quic = False
        memory_total = None
        if sys.platform == 'darwin':
            memory_total = int(subprocess.run(['sysctl', '-n', 'hw.memsize'], capture_output=True, text=True).stdout) / 1073741824
        elif Path('/proc/meminfo').exists():
            memory_total = int(Path('/proc/meminfo').read_text().split()[1]) / 1048576
        commit = self.args.commit or first_line(['git', '-C', str(self.source), 'rev-parse', 'HEAD'])
        here = Path(__file__).resolve().parent

        def sha256(path):
            return hashlib.sha256(Path(path).read_bytes()).hexdigest() if Path(path).is_file() else None

        cgroup_dir, cgroup_max = self.cgroup
        return {
            'recorded_at': time.strftime('%Y-%m-%dT%H:%M:%S%z'),
            'label': self.args.label, 'commit': commit, 'scenarios': scenarios,
            'platform': platform.platform(), 'machine': platform.machine(),
            'logical_cpus': os.cpu_count(), 'memory_total_gib': memory_total,
            'memory_limit': {'cgroup': str(cgroup_dir) if cgroup_dir else None,
                             'cgroup_memory_max_gib': cgroup_max / 1073741824 if cgroup_max else None},
            'tool_sha256': {'measure.py': sha256(here / 'measure.py'), 'launcher.cpp': sha256(here / 'launcher.cpp'),
                            'launcher': sha256(self.launcher)},
            'sodium': {'url': self.sodium_url, 'sha256': self.sodium_sha256},
            'samples': {'short': self.args.samples, 'short_warmup': 1, 'long': self.args.long_samples,
                        'comment_probe': self.args.comment_samples},
            'jobs': self.args.jobs or 'serial', 'cache_mode': self.args.cache_mode, 'quic_transport_enabled': quic,
            'preset': self.args.preset, 'build_dir': str(self.build_dir), 'deps_dir': str(self.deps_dir),
            'cmake_args': self.args.cmake_arg, 'env_overrides': self.args.env,
            'tools': {
                'cmake': first_line(['cmake', '--version']), 'ctest': first_line(['ctest', '--version']),
                'make': first_line(['make', '--version']), 'ninja': first_line(['ninja', '--version']),
                'ccache': first_line(['ccache', '--version']), 'python': sys.version.split()[0],
                'cxx': first_line([cache.get('CMAKE_CXX_COMPILER', 'c++'), '--version']),
            },
            'cmake_cache': cache,
        }


def cmd_run(args):
    scenarios = SCENARIOS if 'all' in args.scenario else args.scenario
    bench = Bench(args)
    try:
        for scenario in scenarios:
            getattr(bench, 'scenario_' + scenario.replace('-', '_'))()
    finally:
        path = bench.out / 'environment.json'
        history = json.loads(path.read_text()) if path.exists() else []
        history.append(bench.environment(scenarios))
        path.write_text(json.dumps(history, indent=2, ensure_ascii=False))


def cmd_summarize(args):
    out = Path(args.out)
    summary = summarize(json.loads((out / 'stages.json').read_text()))
    (out / 'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False))
    markdown = render_markdown(summary)
    (out / 'summary.md').write_text(markdown)
    print(markdown, end='')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    run = sub.add_parser('run', help='按场景测量')
    run.add_argument('--source', required=True, help='被测源码树（建议是独立副本，探针会临时改写其中文件）')
    run.add_argument('--out', required=True, help='结果目录；同一目录内阶段名不可重复')
    run.add_argument('--launcher', required=True, help='已编译的 launcher 可执行文件')
    run.add_argument('--scenario', action='append', required=True, choices=SCENARIOS + ['all'])
    run.add_argument('--probe', action='append', choices=list(PROBES), help='probes 场景只跑这些探针（默认全部）')
    run.add_argument('--samples', type=int, default=5, help='短场景正式样本数（另加 1 次预热）')
    run.add_argument('--long-samples', type=int, default=3, help='冷入口、热完整验证的样本数；repro 至少 3 轮')
    run.add_argument('--comment-samples', type=int, default=3, help='注释探针（历史对照）样本数')
    run.add_argument('--preset', default='dev')
    run.add_argument('--build-dir', help='相对 --source 的构建目录，默认 build/<preset>')
    run.add_argument('--deps-dir', help='复用的依赖源码目录，默认 <out>/deps-src（由 fetch 场景填充）')
    run.add_argument('--jobs', type=int, help='传给 cmake --build --parallel；缺省为串行')
    run.add_argument('--cache-mode', default='none', help='记录用的编译缓存模式说明')
    run.add_argument('--cmake-arg', action='append', default=[], help='每次配置附加的 CMake 参数（平台适配）')
    run.add_argument('--env', action='append', default=[], metavar='NAME=VALUE', help='测量进程附加的环境变量')
    run.add_argument('--name-prefix', default='')
    run.add_argument('--commit', help='被测提交；缺省读 --source 的 git HEAD')
    run.add_argument('--label', help='自由文本，例如 "P0 Mac"')
    run.set_defaults(func=cmd_run)
    summ = sub.add_parser('summarize', help='按场景汇总 stages.json')
    summ.add_argument('--out', required=True)
    summ.set_defaults(func=cmd_summarize)
    args = parser.parse_args(argv)
    args.func(args)


if __name__ == '__main__':
    main()
