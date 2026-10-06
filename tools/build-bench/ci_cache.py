#!/usr/bin/env python3
"""Measure real GitHub cache transfers around prepared-source cold builds."""
import argparse
from collections import Counter
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import statistics
import subprocess
import sys
import time
import measure as build_measure


def read(path):
    return json.loads(path.read_text())


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def outputs(values, destination='GITHUB_OUTPUT'):
    if os.environ.get(destination):
        with open(os.environ[destination], 'a') as stream:
            for key, value in values.items():
                stream.write(f'{key}={value}\n')


def source_manifest(source):
    names = subprocess.check_output(['git', 'ls-files', '-z'], cwd=source).decode().split('\0')
    return {name: hashlib.sha256((source / name).read_bytes()).hexdigest()
            for name in names if name and (source / name).is_file()}


def bucket(source):
    env = dict(os.environ)
    env.pop('GITHUB_OUTPUT', None)
    env.pop('CCACHE_CONFIGPATH', None)
    result = subprocess.check_output([sys.executable, str(source / 'scripts/ci-ccache-key.py')], env=env, text=True)
    return dict(line.split('=', 1) for line in result.splitlines())['bucket']


def runner_worker():
    processes = {}
    for line in subprocess.check_output(['ps', '-A', '-o', 'pid=,ppid=,comm='], text=True).splitlines():
        pid, parent, command = line.strip().split(None, 2)
        processes[int(pid)] = (int(parent), command)
    pid = os.getpid()
    while pid in processes:
        parent, command = processes[pid]
        if Path(command).name == 'Runner.Worker':
            return pid
        pid = parent
    raise ValueError('cannot identify this job Runner.Worker for whole-phase resource accounting')


def begin_monitor(out, name, root):
    subprocess.Popen([sys.executable, str(Path(__file__).resolve()), 'monitor', '--out', str(out),
                      '--label', name, '--root-pid', str(root)],
                     stdout=(out / f'{name}-monitor.log').open('w'), stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 5
    while not (out / f'{name}-ci-memory.jsonl').exists():
        if time.monotonic() > deadline:
            raise ValueError('whole-phase resource monitor did not start')
        time.sleep(0.05)


def end_monitor(out, name):
    (out / f'{name}-monitor.stop').touch()
    path = out / f'{name}-ci-memory.json'
    deadline = time.monotonic() + 5
    while not path.exists():
        if time.monotonic() > deadline:
            raise ValueError('whole-phase resource monitor did not finish')
        time.sleep(0.05)
    return read(path)


def monitor(out, name, root):
    cgroup = build_measure.linux_cgroup_limit() if sys.platform.startswith('linux') else (None, None)
    sampler = build_measure.MemorySampler(out / f'{name}-ci-memory.jsonl', cgroup, tree_root=root)
    sampler.start()
    while not (out / f'{name}-monitor.stop').exists():
        time.sleep(0.1)
    write(out / f'{name}-ci-memory.json', sampler.stop())


def measure(state, mode, prefix, index=0, scenario='cold-build'):
    source, out = Path(state['source']), Path(state['out'])
    command = [sys.executable, str(source / 'tools/build-bench/measure.py'), 'run',
               '--source', str(source), '--out', str(out), '--launcher', str(out / 'launcher'),
               '--deps-dir', str(out / 'deps-src'), '--scenario', scenario,
               '--preset', state['preset'], '--jobs', str(state['jobs']), '--cache-mode', mode,
               '--long-samples', '1', '--sample-start', str(index), '--name-prefix', prefix,
               '--commit', state['revision'], f'--cmake-arg=-DREALMMESH_CCACHE_DIR={state["cache_dir"]}',
               '--cmake-arg=-DREALMMESH_CCACHE_MAX_SIZE=2GiB']
    subprocess.run(command, cwd=source, check=True)
    return read(out / 'stages.json')[-1]


def setup(args):
    if os.environ.get('GITHUB_ACTIONS') != 'true' or os.environ.get('GITHUB_EVENT_NAME') != 'workflow_dispatch':
        raise ValueError('cache acceptance publication requires an explicitly dispatched maintenance run')
    source, out = args.source.resolve(), args.out.resolve()
    if (out / 'ci-state.json').exists():
        raise ValueError('use a fresh owned measurement directory')
    out.mkdir(parents=True, exist_ok=True)
    outputs({'CI_CCACHE_OUT': out}, 'GITHUB_ENV')
    cache = source / '.cache/ccache'
    if cache.resolve() != cache or source.resolve() != Path(os.environ['GITHUB_WORKSPACE']).resolve():
        raise ValueError('only the owned checkout compiler cache may be reset')
    free = shutil.disk_usage(out).free
    if free < 8 * 1024 ** 3:
        raise ValueError('need at least 8GiB free for sources, complete products, fixtures, logs and 2GiB cache')
    (out / 'tmp').mkdir()
    os.environ['TMPDIR'] = str(out / 'tmp')
    state = {'source': str(source), 'out': str(out), 'cache_dir': str(cache),
             'preset': args.preset, 'jobs': args.jobs, 'platform': platform.system(),
             'revision': subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=source, text=True).strip(),
             'run_id': os.environ['GITHUB_RUN_ID'], 'run_attempt': os.environ['GITHUB_RUN_ATTEMPT'],
             'ref': os.environ['GITHUB_REF'], 'initial_free_bytes': free,
             'runner_worker_pid': runner_worker(), 'timings': {}}
    write(out / 'source-manifest.json', source_manifest(source))
    write(out / 'ci-state.json', state)
    subprocess.run(['c++', '-std=c++17', '-O2', str(source / 'tools/build-bench/launcher.cpp'),
                    '-o', str(out / 'launcher')], check=True)
    measure(state, 'ON', 'prepare-', scenario='fetch')
    build = Path(subprocess.check_output([str(source / 'scripts/build-dir.sh'), '--preset', args.preset],
                                        cwd=source, text=True).strip()).resolve()
    if not build.is_relative_to(source / 'build'):
        raise ValueError('measurement build directory must belong to this checkout')
    state['build_dir'] = str(build)
    state['bucket'] = bucket(source)
    state['prefix'] = f'ccache-acceptance-{state["bucket"]}-{state["run_id"]}-{state["run_attempt"]}'
    state['seed_key'] = state['prefix'] + '-seed'
    shutil.rmtree(build)
    if cache.exists():
        shutil.rmtree(cache)
    cache.mkdir(parents=True, exist_ok=True)
    state['timings']['seed_start'] = time.perf_counter_ns()
    write(out / 'ci-state.json', state)
    begin_monitor(out, 'seed', state['runner_worker_pid'])
    outputs({'cache_dir': cache, 'unavailable_key': state['prefix'] + '-unavailable'})
    outputs({'CI_CCACHE_OUT': out, 'TMPDIR': out / 'tmp', 'REALMMESH_BUILD_DIR': build,
             'CCACHE_CONFIGPATH': build / 'realmmesh-ccache.conf'}, 'GITHUB_ENV')


def seed(out):
    state = read(out / 'ci-state.json')
    row = measure(state, 'ON', 'seed-')
    state['timings']['seed_build_done'] = time.perf_counter_ns()
    state['seed'] = row
    write(out / 'ci-state.json', state)


def test_count(path):
    text = path.read_text() if path.exists() else ''
    match = re.search(r'100% tests passed(?:, 0 tests failed)? out of (\d+)', text)
    if not match or re.search(r'\*\*\*(Skipped|Not Run)', text):
        raise ValueError('requires the complete test suite without skips')
    return int(match[1])


def verified(out):
    state = read(out / 'ci-state.json')
    state['seed_test_count'] = test_count(out / 'seed-tests.log')
    if state['platform'] == 'Linux':
        report = Path(state['build_dir']) / 'acceptance/linux-login-chain-m1-m4.md'
        if len(re.findall(r'^\| .* \| PASS \|$', report.read_text(), re.M)) != 5:
            raise ValueError('Linux publication requires QUIC and all M1-M4 groups')
        shutil.copyfile(report, out / 'seed-linux-login-chain-m1-m4.md')
    if state.get('unavailable_restore_outcome') != 'failure':
        raise ValueError('controlled restore failure was not exercised')
    state['seed_verified'] = True
    write(out / 'ci-state.json', state)
    outputs({'seed_key': state['seed_key']})


def start(out, mode, pair):
    state = read(out / 'ci-state.json')
    if not state.get('seed_verified') or not state.get('seed_remote_hit'):
        raise ValueError('measurement requires a tested, remotely readable compatible seed')
    if (out / 'ci-current.json').exists():
        raise ValueError('previous sample is incomplete; retain it and stop')
    if source_manifest(Path(state['source'])) != read(out / 'source-manifest.json'):
        raise ValueError('source changed after the measurement freeze')
    rows = read(out / 'ci-samples.json') if (out / 'ci-samples.json').exists() else []
    if any(row['pair'] == pair and row['mode'] == mode for row in rows):
        raise ValueError('duplicate sample; do not replace raw evidence')
    build, cache = Path(state['build_dir']), Path(state['cache_dir'])
    if build.exists():
        shutil.rmtree(build)
    if mode == 'ON':
        shutil.rmtree(cache)
        cache.mkdir(parents=True)
    current = {'pair': pair, 'mode': mode, 'timings': {'start': time.perf_counter_ns()}, 'actions': {}}
    write(out / 'ci-current.json', current)
    begin_monitor(out, f'{mode}-{pair}', state['runner_worker_pid'])
    current['bucket'] = bucket(Path(state['source'])) if mode == 'ON' else state['bucket']
    current['timings']['key_done'] = time.perf_counter_ns()
    if current['bucket'] != state['bucket']:
        raise ValueError('compiler/toolchain/dependency compatibility bucket changed')
    write(out / 'ci-current.json', current)
    outputs({'seed_key': state['seed_key'], 'save_key': state['prefix'] + f'-pair-{pair}'})


def build(out):
    state, current = read(out / 'ci-state.json'), read(out / 'ci-current.json')
    current['stage'] = measure(state, current['mode'], current['mode'] + '-', current['pair'])
    current['timings']['build_done'] = time.perf_counter_ns()
    write(out / 'ci-current.json', current)


def mark(out, label, outcome, hit):
    is_seed = label.startswith('seed_') or label == 'unavailable_restore'
    path = out / ('ci-state.json' if is_seed else 'ci-current.json')
    value = read(path)
    value['timings'][label] = time.perf_counter_ns()
    if label == 'unavailable_restore':
        value['unavailable_restore_outcome'] = outcome
    elif label == 'seed_remote_check':
        value['seed_remote_hit'] = hit == 'true' and outcome == 'success' and value.get('seed_save_outcome') == 'success'
        value['seed_memory'] = end_monitor(out, 'seed')
        if not value['seed_remote_hit']:
            write(path, value)
            raise ValueError('seed save did not produce a readable remote snapshot')
    elif label == 'seed_save_done':
        value['seed_save_outcome'] = outcome
    elif not is_seed:
        if label == 'restore_done':
            value['actions'].update(restore_outcome=outcome, restore_hit=hit)
        elif label == 'save_done':
            value['actions']['save_outcome'] = outcome
        elif label == 'lookup_done':
            value['actions']['saved_lookup_hit'] = hit
            value['actions']['saved_lookup_outcome'] = outcome
    write(path, value)


def finish(out):
    current = read(out / 'ci-current.json')
    current['timings']['finish'] = time.perf_counter_ns()
    row = current.pop('stage')
    row.update(current)
    row['net_wait_s'] = (row['timings']['finish'] - row['timings']['start']) / 1e9
    row['build_memory'] = row.get('memory')
    row['memory'] = end_monitor(out, f'{row["mode"]}-{row["pair"]}')
    before, after = row.get('ccache_before') or {}, row.get('ccache_after') or {}
    row['cache_delta'] = {key: after.get(key, 0) - before.get(key, 0) for key in set(before) | set(after)}
    timing = row['timings']
    row['cost_s'] = {'key_and_monitor': (timing['key_done'] - timing['start']) / 1e9,
                     'configure_build': row['wall_s'],
                     'other_overhead': row['net_wait_s'] - row['wall_s']}
    if row['mode'] == 'ON':
        row['cost_s'].update(restore=(timing['restore_done'] - timing['key_done']) / 1e9,
                             save=(timing['save_done'] - timing['save_start']) / 1e9,
                             saved_check=(timing['lookup_done'] - timing['save_done']) / 1e9)
    log = (out / row['log']).read_bytes()
    names = re.findall(r'^\s+CC\s+(.+\.lo)$', log.decode(errors='replace'), re.M)
    row['sodium_compiled_sources'] = sorted(set(names))
    row['sodium_compile_requests'] = len(names)
    row['sodium_compile_duplicates'] = {key: count for key, count in Counter(names).items() if count > 1}
    row['log_sha256'] = hashlib.sha256(log).hexdigest()
    rows = read(out / 'ci-samples.json') if (out / 'ci-samples.json').exists() else []
    rows.append(row)
    write(out / 'ci-samples.json', rows)
    (out / 'ci-current.json').unlink()
    print(json.dumps({'pair': row['pair'], 'mode': row['mode'], 'net_wait_s': row['net_wait_s'],
                      'build_s': row['steps'][-1]['wall_s'], 'compile_count': row['compile_count']}))


def resource_errors(memory, system, label):
    memory = memory or {}
    errors = []
    if (memory.get('min_available_mib') or 0) < 1024:
        errors.append(f'{label}: available memory below 1GiB or unavailable')
    if memory.get('swap_growth_mib') != 0:
        errors.append(f'{label}: swap grew or measurement unavailable')
    if system == 'Darwin' and memory.get('max_pressure_level') != 1:
        errors.append(f'{label}: macOS pressure is not normal')
    if system == 'Linux' and memory.get('oom_kills') != 0:
        errors.append(f'{label}: OOM occurred or accounting unavailable')
    return errors


def summarize(out):
    rows = read(out / 'ci-samples.json') if (out / 'ci-samples.json').exists() else []
    state = read(out / 'ci-state.json') if (out / 'ci-state.json').exists() else {}
    errors = []
    if (out / 'ci-current.json').exists():
        current = read(out / 'ci-current.json')
        end_monitor(out, f'{current["mode"]}-{current["pair"]}')
        errors.append('incomplete sample retained in ci-current.json')
    if (out / 'seed-ci-memory.jsonl').exists() and not (out / 'seed-ci-memory.json').exists():
        state['seed_memory'] = end_monitor(out, 'seed')
        write(out / 'ci-state.json', state)
    if not state.get('seed_verified') or not state.get('seed_remote_hit') or state.get('unavailable_restore_outcome') != 'failure':
        errors.append('tested remote seed or unavailable-restore continuation missing')
    errors.extend(resource_errors(state.get('seed_memory'), state.get('platform'), 'seed'))
    if state.get('platform') == 'Linux':
        for prefix in ('seed', 'on'):
            report = out / f'{prefix}-linux-login-chain-m1-m4.md'
            if not report.exists() or len(re.findall(r'^\| .* \| PASS \|$', report.read_text(), re.M)) != 5:
                errors.append(f'{prefix} Linux QUIC/M1-M4 correctness incomplete')
    for mode in ('seed', 'off', 'on'):
        path = out / f'{mode}-tests.log'
        try:
            count = test_count(path)
        except ValueError as error:
            errors.append(f'{mode} correctness: {error}')
        else:
            if count != state.get('seed_test_count'):
                errors.append(f'{mode} correctness test set changed')
    counts = Counter((row['pair'], row['mode']) for row in rows)
    pairs = sorted({row['pair'] for row in rows})
    if not 3 <= len(pairs) <= 5 or any(counts[pair, mode] != 1 for pair in pairs for mode in ('OFF', 'ON')):
        errors.append('need 3 to 5 complete unique OFF/ON pairs')
    workload_keys = ('compiled_sources', 'linked_outputs', 'sodium_compiled_sources')
    for key in workload_keys:
        expected = Counter(rows[0].get(key, [])) if rows else Counter()
        if not expected or any(Counter(row.get(key, [])) != expected for row in rows):
            errors.append(f'workload differs or is missing: {key}')
    for row in rows:
        if row.get('bucket') != state.get('bucket'):
            errors.append(f"pair {row['pair']}: compatibility bucket changed")
        errors.extend(resource_errors(row.get('memory'), state.get('platform'), f'pair {row["pair"]} {row["mode"]}'))
        if row.get('exit') != 0 or row.get('compile_count') != len(row.get('compiled_sources', [])):
            errors.append(f"pair {row['pair']}: failed build or incomplete work accounting")
        if row['mode'] == 'ON':
            delta = row.get('cache_delta', {})
            hits = delta.get('direct_cache_hit', 0) + delta.get('preprocessed_cache_hit', 0)
            if delta.get('cache_miss', -1) != 0 or hits != row.get('compile_count', 0):
                errors.append(f"pair {row['pair']}: not a complete hot native rebuild")
            actions = row.get('actions', {})
            if actions.get('restore_outcome') != 'success' or actions.get('restore_hit') != 'true':
                errors.append(f"pair {row['pair']}: restore did not return the exact compatible snapshot")
            if actions.get('save_outcome') != 'success' or actions.get('saved_lookup_hit') != 'true' or actions.get('saved_lookup_outcome') != 'success':
                errors.append(f"pair {row['pair']}: save did not produce a remotely readable snapshot")
    grouped = {mode: [row['net_wait_s'] for row in rows if row['mode'] == mode]
               for mode in ('OFF', 'ON')}
    medians = {mode: statistics.median(values) if values else None for mode, values in grouped.items()}
    reduction = 1 - medians['ON'] / medians['OFF'] if all(medians.values()) else None
    complete = [pair for pair in pairs if counts[pair, 'OFF'] == counts[pair, 'ON'] == 1]
    faster = sum(next(row['net_wait_s'] for row in rows if row['pair'] == pair and row['mode'] == 'ON') <
                 next(row['net_wait_s'] for row in rows if row['pair'] == pair and row['mode'] == 'OFF')
                 for pair in complete)
    if faster <= len(complete) / 2:
        errors.append('a majority of paired rebuilds must improve')
    summary = {'medians': medians, 'reduction': reduction, 'errors': errors,
               'range_s': {mode: [min(values), max(values)] if values else None for mode, values in grouped.items()},
               'pair_reductions': [1 - next(row['net_wait_s'] for row in rows if row['pair'] == pair and row['mode'] == 'ON') /
                                  next(row['net_wait_s'] for row in rows if row['pair'] == pair and row['mode'] == 'OFF')
                                  for pair in complete],
               'first_fill': state.get('seed'), 'first_upload_s': (state.get('timings', {}).get('seed_save_done', 0) -
                                    state.get('timings', {}).get('seed_save_start', 0)) / 1e9,
               'seed_memory': state.get('seed_memory'),
               'accepted': not errors and reduction is not None and reduction >= 0.2}
    (out / 'ci-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps({key: value for key, value in summary.items() if key != 'first_fill'}))
    markdown = '# CI 编译缓存验收\n\n' + '```json\n' + json.dumps({key: value for key, value in summary.items() if key != 'first_fill'}, indent=2) + '\n```\n'
    (out / 'ci-summary.md').write_text(markdown)
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as stream:
            stream.write(markdown)
    return 0 if summary['accepted'] else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['setup', 'seed', 'verified', 'start', 'build', 'mark', 'finish', 'monitor', 'summarize'])
    parser.add_argument('--out', required=True, type=Path)
    parser.add_argument('--source', type=Path)
    parser.add_argument('--preset', default='dev')
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--mode', choices=['OFF', 'ON'])
    parser.add_argument('--pair', type=int, choices=range(1, 6))
    parser.add_argument('--label')
    parser.add_argument('--outcome', default='')
    parser.add_argument('--hit', default='')
    parser.add_argument('--root-pid', type=int)
    args = parser.parse_args()
    required = {'setup': ['source'], 'start': ['mode', 'pair'],
                'mark': ['label'], 'monitor': ['label', 'root_pid']}
    for name in required.get(args.command, []):
        if getattr(args, name) is None:
            parser.error(f'{args.command} requires --{name.replace("_", "-")}')
    if args.jobs < 1:
        parser.error('--jobs must be positive')
    if args.command == 'setup':
        setup(args)
    elif args.command == 'start':
        start(args.out, args.mode, args.pair)
    elif args.command == 'mark':
        mark(args.out, args.label, args.outcome, args.hit)
    elif args.command == 'monitor':
        monitor(args.out, args.label, args.root_pid)
    else:
        return {'seed': seed, 'verified': verified, 'build': build,
                'finish': finish, 'summarize': summarize}[args.command](args.out)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
