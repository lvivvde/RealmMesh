# #119 夹具等待证据

起点：`10dc2fb3438e9c8f8199b9dd229ae536437ecdec`，加本轮
`tests/scripts/ccache_test.py` 改动。正式源码 SHA 在各平台 `samples.json` 中。
正式对照两侧断言相同，before 只把输入年龄等待恢复为固定 1.05 秒。

- `mac/samples.json` 与逐次日志：双方预热各一次、五组交替配对，全部六例 Native。
- Linux 初组和重新取得的正式组分别保存；初组与 Mac 完整正确性运行重叠，不进正式统计。
- `native-before.log`：沙箱内初始 cProfile，测量 CLI 的系统内存读取被禁止，一例失败；不计入通过样本。
- `native-unsandboxed.log`：沙箱外原始六例 cProfile 诊断，全部通过。
- `native-after.log`：首次输入年龄方案的六例 cProfile 诊断，尚未补强全部 direct hit 断言；不进正式统计。
- `no-wait.log`：在临时副本直接删除等待、沿用旧断言的诊断；通过也不能验证时间安全，不作为采用方案。
- `no-wait-strengthened.log`：正常文件时间下即使补强断言，删等待的诊断仍能通过；不冒充粗粒度时间边界验证。
- `timestamp-boundary.py` 与 `boundary-*.log`：输入头 mtime 设为略未来 0.8 秒的公共工具边界；删等待失败，固定等待和输入年龄等待通过。
- `pairs.py`：本轮实际使用的短场景驱动，逐次保存单调墙钟、退出码、日志、工具版本和脚本 SHA；失败即停，失败资产保留。
- `before-wait.patch`：从正式 after 脚本恢复对照等待的补丁。
- `verification.json`：两平台 QUIC 完整 667/667 与单次 CTest 时间；Linux 的完整入口另记实际墙钟。
- `linux-full.py` / `linux-full.json` / `linux-configure.log.gz`：新隔离 Linux 源码以固定依赖、Ninja/OFF、2 路构建后串行全量测试的驱动、退出码与配置证据；Mac 为现有稳定产物、AUTO、8 路。两者不组成性能配对。

原始工作目录：Mac `/private/tmp/realmmesh-119-fixture-assessment`；Lima
`/home/edwin.guest/code/.bench/issue119-fixture-20261007`。cProfile 的二进制
profile 与已保留的原始/删等待诊断源码另外保存为仓库中的
[`diagnostic-profiles.tar.gz`](diagnostic-profiles.tar.gz)，SHA 见
[校验清单](manifest.json)；初始 after 诊断源码没有单独冻结，该 profile 不作为
正式来源证据。完整 CTest 日志压缩保存为 `*-full.log.gz`。仓库中的诊断秒数来自该
profile 与原始日志，未用于正式墙钟统计。全量运行只有每平台一次正确性检查，不构成三组完整性能配对；
本轮没有采集完整进程树资源时间线，不作新的完整性能/资源验收声明。

## 复现局部配对

在包含本轮提交的源码检出中，创建新的专用目录 `$fixture_bench`（不得覆盖上述
证据目录），保持 cmake、Ninja、C/C++ 编译器和有效 ccache ≥4.8 在 PATH。

```bash
cp docs/research/assets/build-fixture-waits-119-2026-10-07/pairs.py "$fixture_bench/"
cp tests/scripts/ccache_test.py "$fixture_bench/ccache-paired-after.py"
cp tests/scripts/ccache_test.py "$fixture_bench/ccache-paired-before.py"
patch -d "$fixture_bench" -p1 < docs/research/assets/build-fixture-waits-119-2026-10-07/before-wait.patch
python3 "$fixture_bench/pairs.py" --source "$PWD" --out "$fixture_bench/results"
```

先核对两份脚本 SHA 与 `samples.json`；两侧补强断言一致，只允许等待实现不同。
Native 用例内部仍每例创建独立源码、构建和缓存；套件之间也不留编译产物。
Mac 需要能读取 `sysctl hw.memsize` 的运行环境；测试夹具只用本地小工程与对象
缓存，不连接任何数据库。两平台取样不得与另一平台构建/测试或其他高负载任务重叠。

完整正确性入口是 `./scripts/build.sh --jobs 8`（本次 Mac）与
`./scripts/build.sh --jobs 2`（本次 Lima）；Lima 先以 P6 已核对的依赖源码配置
新的隔离检出，使用已有 `.tools` 只读工具链接，TMPDIR 位于新验证目录的磁盘路径。
Linux 主检出、P6 检出和历史构建/证据未改。两机都只使用 poll 监听后端。

复现时间边界诊断时，先准备上述 before/after 两份脚本，再运行：

```bash
python3 docs/research/assets/build-fixture-waits-119-2026-10-07/timestamp-boundary.py --fixture "$fixture_bench/ccache-paired-after.py" --source "$PWD" --work "$fixture_bench/boundary" --native --ccache "$(command -v ccache)"
```

把 `--fixture` 改为 before 脚本应同样通过；仅在新的临时副本将固定 sleep 改为零，
以相同命令运行应在 direct hit 断言失败。真实编译延迟足以使略未来时间自然老化时，
该删等待诊断可能不触发；日志必须保留实际结果，不以某个机器的失败保证所有机器失败。
