# 日常反馈与完整验证：构建测试入口决策

关联[确定日常反馈与完整验证的构建测试入口](https://github.com/lvivvde/RealmMesh/issues/114)。用户已通过 Q1–Q5 选择入口、范围、配置、完整验证触发条件与并行度，并最终确认完整方案及正式记录。本文件是实施约定；[#125](https://github.com/lvivvde/RealmMesh/issues/125)（P2b）已按此落地，实际选择与失败行为见文末[实施记录](#实施记录125)。

## 入口与使用场景

| 场景 | 入口 | 配置、构建与测试范围 |
| --- | --- | --- |
| 普通编辑循环 | `./scripts/test-fast.sh` | 每次标准 dev 配置；构建全部 Unit 所需目标及真实依赖；运行全部 unit 用例 |
| 聚焦一个 Unit 目标 | `./scripts/test-fast.sh --target lua_runtime_test` | 每次配置；仅构建已注册 Unit 目标及依赖；运行该目标的全部用例 |
| 进一步聚焦用例 | 上述入口追加 `--test-regex <regex>` | 在所选范围内筛选；空结果必须失败 |
| 连续编辑 | `./scripts/test-watch.sh`，支持相同聚焦参数和 `--once` | 每轮调用快速入口，启动即跑；新变更顺序触发下一轮 |
| 阶段完成及公共头、跨模块、网络、协议、存储行为变化 | `./scripts/build.sh` | 每次标准 dev 配置、构建 ALL、完整 CTest |
| 合并验证 | 现有双平台 CI | 继续配置、构建 ALL、完整 CTest；Linux 继续 QUIC 与 M1–M4 接入验收 |

快速入口默认 CTest `-j 4`，提供 `--test-jobs <正整数>`，可退回 1 路。4 是默认上限，不自动按 CPU 数增大；用户显式调高时由其机器资源约束。完整入口与 CI 显式 `-j 1`，避免继承 CTEST_PARALLEL_LEVEL 而改变语义。编译并行、生成器和编译缓存另由工具策略决策确定。

每次配置沿用标准 preset 与现有系统 CMake / .tools/cmake 回退；ctest 与选中的 cmake 使用同一分发路径。首次使用快速入口可自行配置，不要求先跑完整测试。继续按已有约定生成 compile_commands 入口；不新增脚本自行判断配置过期的指纹系统。

## 目标与测试身份

Unit 构建范围由 `realm_add_gtest` / `realm_add_lua_test` 注册时的最终分类自动收集，形成一个 Unit 聚合目标；名称可在实施时确定。Lua 套件依赖 realm_lua_cli，聚合时只需构建一次。已有 LIBS、DEPENDS、TLS 测试身份生成等构建依赖必须保留。全量 ALL 仍构建生产服务、负载工具及所有测试。

`--target` 首批仅接受配置时登记的 Unit 构建目标。Lua 可通过 realm_lua_cli 聚焦其已注册 Unit 套件。目标清单与精确身份标签由注册帮助函数生成，不维护第二份手写列表，不从文件改动猜测受影响测试，不用 GTest 名称猜构建目标。

每个用例同时保留原有 unit/integration/lua 等标签及属性，并增加精确目标身份标签；LABELS 必须作为一个正确的列表值传递。先验证目标的 Unit 身份，再通过转义后的精确身份标签筛选；默认精确筛选 unit，额外正则只收窄所选范围。不得提供能绕过范围的任意 CTest 参数透传，例如 --rerun-failed 会忽略其他选择条件，可能重跑上次全量留下的 integration 失败。

未知或非 Unit 目标、无用例、二进制缺失、配置或构建失败均返回非零；构建失败后不运行旧测试二进制。报告选择范围、通过/失败/跳过及各阶段时间，--once 返回实际失败状态；循环模式报告失败后等待新变更。跳过不能描述为全部验证通过。

这些筛选与失败行为可用 CMake 3.20 已有的 `-L`、`-R`、`-j N`、`--no-tests=error` 实现；不因本方案新增 Python 或提高最低 CMake 版本。[CTest 3.20 官方手册](https://cmake.org/cmake/help/v3.20/manual/ctest.1.html)。

## Watch 与完整验证边界

Watch 复用快速入口的配置、选择和退出约定。关注 framework、game、apps、tools、tests、configs、proto、第三方构建输入，以及根 CMake、preset、相关脚本；排除生成目录与编辑器临时文件，避免自身构建触发循环。新增、删除、移动均需识别；第一轮以及配置/构建/测试期间保存的变更也必须进入下一轮。保留 fswatch、inotify、轮询回退，不同时启动重叠构建，不为了监听增加复杂依赖推断系统。

本地阶段完成或上述高影响变更运行完整入口，CI 兜底不能替代该约定。现有 CI 触发是 PR、main push 和手动运行；普通功能分支 push 并不自动运行。macOS CI 保持 TLS/TCP，Linux 是 QUIC gate；开发 Mac 按实际 libmsquic 安装和配置输出判断能力。etcd、MongoDB、磁盘等完整测试前置条件继续按已有说明满足；自动测试只用隔离数据库，不能接共享开发 MongoDB。

现有部分 integration 标记 RUN_SERIAL，部分未标记；本次有限范围核查未发现统一资源预算或资源锁。首批不扩大完整测试并行。若以后要放开，先审计端口、etcd/Mongo 进程数、磁盘和共享状态，补足资源约束后再单独决定。Unit 标签本身也不是每个用例绝无外部资源的证明，启用 4 路前须在当前代码双平台验证。

## 证据与耗时口径

源码调查 HEAD：14c19d08d42ea4653914c91a82d6b971541c6cb6。当前 build.sh 做 ALL + 全量 CTest；test-watch.sh 做 ALL + unit，所以现有 watch 只减少测试执行，仍遍历并构建所有目标。现有 watch 还要求预配置、遗漏部分输入路径、轮询不能发现删除、第一轮期间变更可能漏掉；首批入口改造需要一起解决这些契约问题。

调查时注册帮助函数内共 70 个 GTest 注册调用：58 个 unit、12 个 integration，另有 2 个 Lua Unit 套件；条件 QUIC 目标是否存在由配置决定。这是注册调用数量，不能当作当前 CTest 用例数。测试发现、链接与配置具有各自成本。

[冻结基线](build-baseline-2026-10-02.md)的热配置中位数 macOS 1.37 秒、Linux 0.98 秒；稳定无改动 ALL 构建为 6.20/1.85 秒。Lua 实现变更仍有 53 次链接；分头不能消除所有链接等待，局部构建是独立的改善手段。Unit 串行中位耗时 8.50/4.37 秒；全量 CTest 单样本 362.80/281.74 秒，Linux 后者有两项失败，不能作通过验收结果。

为测试并行选择补测同一冻结快照、已有二进制，无新配置或编译，命令为 `ctest --preset dev -L unit -j 4`，清除 MongoDB 连接环境和继承的测试并行环境。两平台各三次、每次 494 用例均通过：

| 平台 | 4 路中位墙钟时间 | 范围 |
| --- | ---: | ---: |
| macOS | 2.063 秒 | 2.051–2.100 秒 |
| Linux Lima | 1.076 秒 | 1.069–1.082 秒 |

串行数据来自较早基线，未与本补测交错控制；这里只支持采用适度并行的决策，不承诺固定倍数。补测不是当前 HEAD 的测试全集，也不是新入口总时间。完整实验条件、工具、硬件、快照含脏补丁等沿用基线；日志与逐次退出码保存在[原始补测资产](assets/fast-feedback-2026-10-02/unit-parallel-probe.tar.gz)，[校验清单](assets/fast-feedback-2026-10-02/manifest.json)。

实施后分开报告 configure、build、test 和总墙钟时间，区分默认全部 Unit、聚焦 Unit 与完整验证；快速入口时间不冒充完整验证提速。无改动、代表性 cpp、公共头改动都要同时记录构建目标与实际编译/链接范围。验收量化阈值由“锁定优化实施顺序与三类耗时验收标准”确定。

## 实施验收与维护成本

1. 干净 build/dev 上快速入口可启动；默认构建并运行当前配置所有 Unit，聚焦入口只运行对应范围；切换可用 QUIC 条件时注册范围同步变化。
2. 新 Unit 自动进入聚合；非 Unit 不能聚焦；无匹配、构建失败和缺二进制不产生假绿；Lua/TLS 依赖正常生成。
3. 当前代码在 macOS 和 Linux 上重复验证 4 路 Unit；串行回退有效，环境变量不能改动完整串行约定。
4. Watch 三种模式覆盖删除、移动、首轮与构建期间变更，不重叠、不漏跑，--once 和直接快速入口选择一致。
5. 本地完整 CTest、双平台 CI、Linux QUIC/M1–M4 的覆盖保持；历史 Linux 基线失败独立处理，记录修复后结果。
6. 文档随代码更新 tests/README、入口帮助与相关开发说明。只新增一个快速入口，注册信息统一维护，watch 复用；不创建手写目标表、自动变更测试映射或配置缓存判断器。

## 实施记录（#125）

以下是 P2b 的实际选择；上文约定未改动的部分不再重复。耗时与工作量见[构建优化结果的 P2b 节](build-optimization-results.md#p2b快速入口与并行预算125)。

### 注册与身份

- Unit 聚合目标名为 `realmmesh_unit_tests`，由 `tests/cmake/test_helpers.cmake` 在注册时追加依赖：GTest 目标本身；Lua 套件为 `realm_lua_cli`，多个套件只登记一次。原有 LIBS、DEPENDS 与 TLS 身份生成依赖不变。
- 每次注册的分类标签必须恰好是 `unit`、`integration` 之一，否则配置失败；Lua 的 `lua` 筛选标签保留。
- 精确身份标签为 `target=<构建目标>`，Lua 套件为 `target=realm_lua_cli`。CMake 3.20 的 `gtest_discover_tests` 会把 PROPERTIES 里的列表值拆平，所以标签不经它传递，而是由注册时生成的 `<目标>_realmmesh_labels.cmake` 追加到目录的 `TEST_INCLUDE_FILES`，对 `<目标>_TESTS` 整体设置 `LABELS`。二进制尚未构建时只有占位用例 `<目标>_NOT_BUILT`，它也带同样的标签，在所选范围内表现为失败，不会悄悄缺席。
- 根 CMakeLists 在全部测试目录之后写出登记清单 `<构建目录>/realmmesh-unit-targets.txt`，每行 `<目标>=<产物路径>`（生成期求值）。`--target` 按字面查这份清单，不当正则。`BUILD_TESTING=OFF` 时删除清单。

### 选择与并行

| 入口 | 构建 | CTest 选择 | CTest 并行 |
| --- | --- | --- | --- |
| `test-fast.sh` | `--target realmmesh_unit_tests` | `-L '^unit$'` | `--test-jobs`，默认 4 |
| `test-fast.sh --target T` | `--target T` | `-L '^target=<转义后的 T>$' -LE '^integration$'` | 同上 |
| 上述追加 `--test-regex R` | 不变 | 另加 `-R R`，只收窄 | 同上 |
| `build.sh`、CI | ALL | 全部 | 显式 `-j 1` |

- 标签筛选是子串匹配的正则，所以一律加锚点，例如 `beta.x_test` 不会选中 `betaxx_test`。
- 快速入口始终带 `--no-tests=error` 与显式 `-j`，不继承 `CTEST_PARALLEL_LEVEL`，也不透传其他 CTest 参数。
- 编译 jobs 由 `scripts/lib/build-jobs.sh` 按[工具决策](build-tool-cache-decisions.md)的预算公式计算，`build.sh`、`test-fast.sh`、`test-watch.sh`（原样转给快速入口）与两份登录链验收脚本共用。
  - 逻辑 CPU 数在 Linux 上与 cgroup v2 `cpu.max` 配额取小；内存取 `/proc/meminfo` 与 cgroup 内存限额中的较小值。读不到就按 1 路。
  - 每次运行打印一行 `build jobs: N (from <来源>; budget …)`。开发 Mac（15 CPU、48 GiB）得 8，Lima（8 CPU、7.73 GiB）得 2。
  - CI 不跑脚本，在 workflow 里显式 `--parallel 2`。
- libsodium 的 ExternalProject 构建与安装命令固定 `make -j1`，并用 `cmake -E env --unset=MAKEFLAGS --unset=MFLAGS --unset=MAKELEVEL` 断开顶层 Make 的 jobserver，顶层 jobs 不乘进外部构建。
- 生成器仍是 Unix Makefiles（`dev`、`dev-make` 两个预设）。链接池随 P4 的 Ninja 实施，这里不做。

### 退出码与失败行为

| 情况 | `test-fast.sh` | 结论行 |
| --- | --- | --- |
| 所选用例全部通过 | 0 | `PASSED: …` |
| 通过但有跳过 | 0 | `passed with skips: …; the skipped tests were not verified.` |
| 配置失败、构建失败、构建后缺二进制、无登记清单 | 1 | `FAILED: …`；构建失败与缺二进制都不运行任何测试 |
| 用例失败、所选范围内没有用例 | 2 | `FAILED: …` |
| 用法错误；`--target` 不是已登记的 Unit 目标；`--test-regex` 为空 | 64 | 打印用法或已登记目标 |

- 每次结束打印 `test-fast: time configure Xs, build Ys, test Zs, total Ts (exit C)`。未到达的阶段记为 `-`。
- `build.sh` 与登录链验收脚本的用法错误（包括非法 `--jobs`）仍返回 2，沿用原有约定。

### Watch

- `test-watch.sh` 每轮调用 `test-fast.sh`，参数原样转交；`--once` 返回该轮的退出码。循环模式下失败只报告，用法错误（64）直接退出。
- 是否有变更以文件快照判定：路径、修改时间、大小，另加“比本轮开始时间戳新”的文件。所以新增、删除、移动都能识别，同一秒内的再次保存也不会漏。
- 每轮开始前记快照，轮次结束后比较，所以第一轮和配置、构建、测试期间保存的变更都会再触发一轮。轮次在前台顺序执行，不会重叠。
- fswatch、inotifywait 只负责尽快唤醒，另有定时复查兜底它们启动前后的空档；没有这两个工具时轮询。
- 排除 `.git`、`__pycache__`、落在监听范围内的构建目录与常见编辑器临时文件。
- SIGINT 或 SIGTERM 打印 `test-watch stopped.` 后以 0 退出。

### 验证

`tests/scripts/test_fast_test.sh`（CTest `TestFastScriptTest.FastEntryWatchAndJobsBudget`，integration）在临时小工程里驱动真实的预设、注册帮助函数与脚本。它覆盖：

- 预算公式各分支，以及 `--jobs`、`CMAKE_BUILD_PARALLEL_LEVEL` 的优先级与非法值；
- 首次未配置即可运行，标签是一个列表值，登记清单正确，新 Unit 目标自动进入聚合；
- 转义后的精确筛选，Lua 聚焦，非 Unit 或未知目标被拒，空选择失败；
- 用例失败、构建失败、缺二进制与跳过时的退出码，以及构建失败时不运行 CTest；
- `build.sh` 的串行 CTest，用户预设；
- watch 的 `--once`，以及首轮期间、修改、新增、移动、删除各触发一轮。编辑器临时文件不触发，轮次不重叠，SIGTERM 正常退出。轮询模式必测，本机装有 fswatch 或 inotifywait 时同样再跑一遍。

## 调查范围

使用 Verify 图证据：项目 Users-edwin-Projects-RealmMesh，generation 2026-10-02T09:57:53Z；注册/入口符号查询已完成相关分页。相关路径覆盖无记录缺口；接入脚本记录的 224 行部分解析已读相邻源码。该结果不是全仓测试资源安全的完整证明。

关键源码：scripts/build.sh:23、scripts/test-watch.sh:21、:46、:99、:126；tests/cmake/test_helpers.cmake:28、:74；CMakePresets.json；.github/workflows/ci.yml；tests/cmake/dev-services-tests.cmake；tests/cpp/game/common/CMakeLists.txt；tests/cpp/framework/service_host/CMakeLists.txt；scripts/run-linux-login-acceptance.sh。测试夹具与 RUN_SERIAL/资源属性另在测试注册和支撑目录作有界核查。未修改业务实现、运行协议或领域术语，无需为本方案新增领域词或 ADR。
