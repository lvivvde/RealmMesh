# build-bench：构建测量工具

构建优化各阶段（R0、P1–P6）共用的测量工具。路线、目标、采样与资源门槛见[实施顺序与验收约定](../../docs/research/build-implementation-acceptance.md)与 [#115 决议](https://github.com/lvivvde/RealmMesh/issues/115#issuecomment-5954537886)；旧[build-optimization-rollout.md](../../docs/research/build-optimization-rollout.md)只作历史。结果追加到 [build-optimization-results.md](../../docs/research/build-optimization-results.md)。它只用于测量，不进 CMake 构建，也不属于日常入口。

| 文件 | 作用 |
| --- | --- |
| `launcher.cpp` | 作为 `CMAKE_<LANG>_{COMPILER,LINKER}_LAUNCHER` 包住每次真实编译/链接；设置 `REALMMESH_BUILD_BENCH_EVENTS` 时写一条事件 JSON：argv、cwd、墙钟、退出码、user/sys 时间、`maxrss_kib`（macOS 字节已换算为 KiB） |
| `measure.py` | 按场景驱动 `cmake`/`ctest`，整体计时并分步记录，统计实际编译/链接名单与次数，每秒采样内存；`summarize` 按场景汇总，`export` 导出可提交的样本资产 |
| `test_measure.py` | `measure.py` 纯函数的单元测试。不按 [tests/README.md](../../tests/README.md) 进 `tests/` 与 CTest：测量工具不属于被测产品，注册进 CTest 会改变各阶段要比较的测试合集；改动本目录时手动运行，见“自测” |

需要 Python 3.9+、C++17 编译器与 `curl`，不依赖第三方包。

## 场景

`--scenario` 可重复，`all` 按下表顺序全跑。短场景（`unit-entry`、`cpp-entry`、`fast-entry`、`fast-cpp-entry`、`probes`）先跑 1 次预热（阶段名带 `-warmup`，`summarize` 不计入），再跑 `--samples` 次（默认 5）；长场景（`cold-entry`、`hot-full`）跑 `--long-samples` 次（默认 3）。一个阶段里的多条命令整体计时，`steps` 另记每步墙钟；总耗时不由各步中位数相加。

| 场景 | 阶段名 | 步骤 | 内容 |
| --- | --- | --- | --- |
| `fetch` | `fetch-configure`、`fetch-sodium` | configure；download | 首次获取，独立报告：删除构建目录，FetchContent 全新获取并配置，把 `_deps/*-src` 复制到 `--deps-dir`（默认 `<out>/deps-src`）；再用 `curl` 下载 libsodium 包到 `<deps-dir>/sodium-download/` 并核对 `third_party/sodium/CMakeLists.txt` 的 SHA256 |
| `cold-entry` | `cold-entry-N` | prepare、configure、build、test | 完整冷入口：删除构建目录，把已核对的 libsodium 包放进 ExternalProject 下载目录（hash 相符即跳过下载），以 `FETCHCONTENT_SOURCE_DIR_<NAME>` 复用依赖源码配置，全量构建，完整 CTest。build 步即已备齐源码的全量构建 |
| `repro` | `repro-configure-N`、`repro-build-N` | configure；build | 冷入口后同目录连续配置 + 构建，至少 3 轮；行内记录构建树生成版本头（`*version*.h/.hpp`）hash 或 mtime 有变化的名单，首次重新配置单独可见 |
| `hot-full` | `hot-full-N` | configure、build、test | 热完整验证：标准配置 + 稳定无操作 ALL 构建 + 完整 CTest |
| `unit-entry` | `unit-entry-N` | configure、build、test | 默认 Unit、无改动：标准配置 + ALL 构建 + `ctest -L unit` |
| `cpp-entry` | `cpp-entry-N`、`cpp-entry-reset-N` | configure、build、test | 默认 Unit、代表 `.cpp` 真实改动：`lua_runtime.cpp` 写入 token 变体后同 `unit-entry`；每个样本后恢复原始字节，以同样的入口回到稳定状态 |
| `fast-entry` | `fast-entry-N`；`fast-entry-setup-…` | test-fast | P2b（#125）的快速入口、无改动：`scripts/test-fast.sh --preset P`（配置 + Unit 聚合目标 + Unit 4 路）一步整体计时。编译并行缺省由脚本按预算决定，`--jobs` 显式传给它。脚本自报的 configure/build/test 分段记在 `fast_times`，实际编译并行记在 `build_jobs`。每次调用先以标准配置（launcher、复用依赖）写好缓存，记为预热组的 setup 阶段，不计入统计 |
| `fast-cpp-entry` | `fast-cpp-entry-N`、`fast-cpp-entry-reset-N` | test-fast | 同 `cpp-entry` 的 token 变体与 reset，入口换成 `test-fast.sh` |
| `probes` | `probe-<探针>-N`、`probe-<探针>-comment-N`，及对应的 `…-reset-N` | build | 每个探针先 token 变体（主指标），再注释变体 `--comment-samples` 次（默认 3，历史对照）；每个样本后恢复原始字节并构建回稳定状态；`--probe` 可限定 |

`unit-entry`、`cpp-entry`、`hot-full`、`cold-entry` 的 CTest 串行（测试并行 1）；`fast-*` 场景的测试并行由 `test-fast.sh` 决定（默认 4）。

探针：`lua-cpp`（`lua_runtime.cpp`）、`lua-hpp`（`lua_runtime.hpp`，Lua 重头内部变化）、`gateway-hpp`（`gateway_runtime.hpp`）、`player-data-hpp`（`player_data_store.hpp`）、`proto`（`envelope.proto`）、`private-hpp`（`game/common/src/envelope_codec.hpp`，私有头）。变体都追加在文件末尾、由原始字节派生，第 N 次与其他次内容不同：

- token 变体改变预处理结果、不改行为：C++ 追加 `inline constexpr int realmmesh_build_bench_<探针>_variant = N;`（外部链接，不触发未使用告警），proto 追加 `message BuildBenchVariantN {}`。
- 注释变体追加 `// build-bench <探针> content-change sample N`，预处理后不变，只用于和旧数据对照。

每个样本都相对原始状态改动：测完写回原始字节，再用同样的步骤构建回稳定状态，记为 `…-reset-N` 阶段（自成一组，可与样本对照）。因此注释变体预处理后与原始相同，后续缓存阶段也不会把上一个变体当基准。每行记录 `variant`：文件、种类、序号、原始与变体 sha256。每次改写前等到下一个整秒：macOS `/usr/bin/make`（GNU Make 3.81）按整秒比较 mtime，与上次产物同秒的改动会漏编。

## 用法

被测源码用独立副本：探针会临时改写其中文件，冷入口会删除其构建目录。

```bash
out=/path/to/bench   # 源码副本、结果都放这里，不放仓库内；Linux 用磁盘目录，不用 tmpfs
mkdir -p "$out/source"
git archive HEAD | tar x -C "$out/source"
ln -s "$PWD/.tools" "$out/source/.tools"   # etcd、Linux MsQuic/MongoDB 等本机工具，需在计时前装好
c++ -std=c++17 -O2 tools/build-bench/launcher.cpp -o "$out/launcher"
python3 tools/build-bench/measure.py run --source "$out/source" --out "$out/result" \
    --launcher "$out/launcher" --scenario all --commit "$(git rev-parse HEAD)" --label "R0 Mac"
python3 tools/build-bench/measure.py summarize --out "$out/result"
python3 tools/build-bench/measure.py export --out "$out/result" --dest docs/research/assets/<目录>/<名>.json \
    --platform "R0 Mac" --root "$out"
```

`export` 保留逐次样本（含预热与 reset）、每步墙钟、编译/链接次数与合计、静态库归档次数、内存摘要、探针变体 hash、由日志重新解析的 CTest 结果、不超过 60 项的编译源码与链接产物名单、`environment.json` 与 `summary.json`，并把 `--root` 前缀换成 `<bench>`；不含命令行、逐调用事件与原始日志。

常用参数：

- `--cmake-arg=-D...`：每次配置都附加的平台适配参数。R0 在 macOS 上需要 `-DOPENSSL_INCLUDE_DIR=/opt/homebrew/opt/openssl@3/include`；P1（#121）起默认即选 Homebrew `openssl@3` 专用前缀，不再需要。
- `--env NAME=VALUE`：测量进程的附加环境，例如 Lima 把 `TMPDIR` 指到磁盘目录。
- `--jobs N`：传给 `cmake --build --parallel`；缺省串行（R0 条件）。`fast-*` 场景传给 `test-fast.sh --jobs`，缺省用脚本的预算。
- `--samples N --sample-start S --no-warmup`：配对交替测量时，每次调用只跑一个样本，序号接续；对所有短场景（含 `probes`）生效。例如前后各先用 `--samples 0` 预热，再按组交替调用 `--samples 1 --sample-start <组号> --no-warmup`。前后两侧用各自的源码副本与 `--out`。
- `--preset`、`--build-dir`：改预设与构建目录后用。
- `--cache-mode`：只做记录，如 `ccache-AUTO-hot`。

工具会清除 `CMAKE_BUILD_PARALLEL_LEVEL`、`CTEST_PARALLEL_LEVEL`、`MAKEFLAGS` 与外部 launcher 变量，并去掉带 MongoDB URI/口令的变量；测试只用夹具自起的隔离 etcd/MongoDB。

## 输出

`--out` 目录中：

- `stages.json`：每个阶段一行，包含 `steps`（每步命令、墙钟、退出码）、总墙钟、退出码、是否预热、起止负载均值、编译/链接次数与时间合计、其中依赖编译次数（源码位于 `_deps` 或源码根之外）、最大单次编译/链接 RSS、实际编译源码与链接产物名单、内存摘要、探针变体 hash、CTest 结果与失败用例。libsodium 的 configure/make 不经 launcher，不计入编译次数，只在日志与时间线里可见。
- `<阶段>.log`、`<阶段>.timeline.jsonl`、`<阶段>-events/`、`<阶段>.memory.jsonl`：原始日志、关键行时间线、逐调用事件与逐秒内存样本。
- `environment.json`：每次 `run` 追加一条：提交、平台、CPU/内存、Linux cgroup 限额、jobs、缓存模式、样本数、QUIC 是否编入、工具版本、`measure.py`/`launcher.cpp`/launcher 的 sha256、libsodium 来源与校验和、关键 CMake 缓存项与适配参数。
- `summary.json` / `summary.md`：`summarize` 生成的分组统计，含原始样本、中位数、最小–最大值、噪声带 max(极差÷中位数, 5%) 与分步统计。

同一 `--out` 内阶段名不可重复；重测换新目录或加 `--name-prefix`。

暂停与续跑：等一个阶段 `DONE` 后对整个进程组发 `kill -TERM -<pgid>`。`measure.py` 收到 SIGTERM 会恢复正在改写的探针文件并写出 `environment.json`；中断阶段的半成品（`<阶段>-events/` 等）删掉后，用同一 `--out` 只跑剩余场景，`stages.json` 接着追加。

内存摘要的口径：

- 进程树 RSS：每秒用 `ps` 合计 `measure.py` 及全部后代进程的 RSS 取峰值；脱离进程树的守护进程与两次采样之间的短进程不在内。launcher 的 RSS 是单进程峰值，不能乘 jobs 推断整体峰值。
- macOS：可用内存 = `vm_stat` 中 free + inactive + speculative + purgeable 页（近似值），压力等级取 `kern.memorystatus_vm_pressure_level`（1 正常、2 警告、4 严重），swap 取 `vm.swapusage`。
- Linux：可用内存取 `/proc/meminfo` 的 MemAvailable 与本进程 cgroup v2 祖先链中最紧 `memory.max` 余量的较小者；swap 取 `/proc/meminfo`，OOM 取 `/proc/vmstat` 的 `oom_kill` 增量。VM 本身的限额即 `MemTotal`。

## 自测

```bash
python3 -m unittest discover tools/build-bench
```
