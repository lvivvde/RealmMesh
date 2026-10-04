# 日常反馈与完整验证：构建测试入口决策

关联[确定日常反馈与完整验证的构建测试入口](https://github.com/lvivvde/RealmMesh/issues/114)。用户已通过 Q1–Q5 选择入口、范围、配置、完整验证触发条件与并行度，并最终确认完整方案及正式记录。本文件是实施约定，尚未新增或修改入口脚本。

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

## 调查范围

使用 Verify 图证据：项目 Users-edwin-Projects-RealmMesh，generation 2026-10-02T09:57:53Z；注册/入口符号查询已完成相关分页。相关路径覆盖无记录缺口；接入脚本记录的 224 行部分解析已读相邻源码。该结果不是全仓测试资源安全的完整证明。

关键源码：scripts/build.sh:23、scripts/test-watch.sh:21、:46、:99、:126；tests/cmake/test_helpers.cmake:28、:74；CMakePresets.json；.github/workflows/ci.yml；tests/cmake/dev-services-tests.cmake；tests/cpp/game/common/CMakeLists.txt；tests/cpp/framework/service_host/CMakeLists.txt；scripts/run-linux-login-acceptance.sh。测试夹具与 RUN_SERIAL/资源属性另在测试注册和支撑目录作有界核查。未修改业务实现、运行协议或领域术语，无需为本方案新增领域词或 ADR。
