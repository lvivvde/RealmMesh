# 测试

**新功能的测试一律进本目录**,按下表归置;注册与标签约定由 `tests/cmake/test_helpers.cmake` 统一保证。

## 归置约定

| 测试什么 | 位置 | 注册方式 | 标签 |
|---|---|---|---|
| C++ 单元测试(进程内,不占端口、不拉进程) | `tests/cpp/<与源码树同路径>/` | `realm_add_gtest` | `unit`(缺省) |
| Lua 业务模块测试 | `tests/lua/<模块>/` | `realm_add_lua_test` | `unit;lua`(缺省) |
| `configs/*.lua` 加载冒烟 | `tests/cpp/framework/service_host/configs_load_smoke_test.cpp` | 随 C++ 单测走 | `unit` |
| 脚本级/跨进程集成测试(bash 驱动真实脚本与 `realm_mesh`,占真实端口) | `tests/scripts/` | `tests/cmake/dev-services-tests.cmake` 的模式(`add_test` + `RUN_SERIAL`) | `integration` |
| C++ E2E/占端口用例 | `tests/cpp/`(目标内) | `realm_add_gtest … LABELS integration` | `integration` |
| CMake 配置期契约(`cmake/*.cmake` 模块的分支，可能拉起子 `cmake` 配置小工程) | `tests/cmake/*_test.cmake` | `tests/cmake/<契约>-contract.cmake` 里 `add_test` 跑 `cmake -P`,由 `tests/cpp/CMakeLists.txt` 引入 | `integration` |

- 分类标签只有两个:`unit` / `integration`,Lua 套件另带的 `lua` 只作筛选;三者定义都在 `test_helpers.cmake` 头注释与根目录 `CONTEXT.md`(Testing 词目)。
- 新增测试必须带标签:常规 GTest / Lua 套件用缺省即可;**占用固定端口或拉起 `realm_mesh` 的目标必须显式 `LABELS integration`**,否则会混进快速子集抢端口。
- 配置脚本不在 Lua 侧测:真实加载路径在 C++(`LuaRuntime` 沙箱 + `LayeredConfigLoader`),由 `configs_load_smoke_test` 覆盖;Lua 测试的火力留给业务逻辑模块(地图 #15 / 调研票 #16 的边界结论)。

## 运行方式

| 场景 | 命令 |
|---|---|
| 日常编辑循环(快速子集) | `./scripts/test-fast.sh`:配置后只构建 Unit 聚合目标 `realmmesh_unit_tests`,4 路运行全部 `unit` 用例 |
| 聚焦一个 Unit 目标 | `./scripts/test-fast.sh --target lua_runtime_test`(Lua 套件用 `--target realm_lua_cli`);再收窄用例加 `--test-regex REGEX` |
| TDD 反馈环(保存即重跑快速入口) | `./scripts/test-watch.sh`(单跑一轮加 `--once`;其余参数原样转给 `test-fast.sh`) |
| 手动快速子集 | `ctest --preset dev -L '^unit$'`(或 `-L '^lua$'` 只筛 Lua 用例、`-L '^target=<构建目标>$'` 只筛一个目标);需先构建 |
| 一键全量(构建 ALL + 全部测试，串行) | `./scripts/build.sh` |
| 仅全量测试 | `ctest --preset dev -j 1` |
| macOS 登录链验收 | `./scripts/run-macos-login-acceptance.sh`（TLS/TCP：本仓客户端没有 QUIC 拨号器，报告另记 Gateway 的 QUIC 监听是否编入；成功链默认重复 3 次，并在所选预设构建目录的 `acceptance/`（默认 `build/dev-ninja/acceptance/`）生成报告和原始日志；换预设加 `--preset NAME`） |
| Linux M1–M4 登录链验收 | `./scripts/run-linux-login-acceptance.sh`（Linux QUIC + TLS/TCP，默认重复关键组 3 次，并在所选预设构建目录的 `acceptance/`（默认 `build/dev-ninja/acceptance/`）生成报告和原始日志；换预设加 `--preset NAME`） |
| 全量兜底 | push / PR 时 GitHub Actions 在 macOS + Linux 双平台以 2 路编译、`ctest --preset dev -j 1` 跑全量(`.github/workflows/ci.yml`) |

入口共同约定(#125,决策与失败行为见 [build-test-entry-decisions.md](../docs/research/build-test-entry-decisions.md#实施记录125)):

- 都接受 `--preset NAME`(默认 `dev`)与 `--jobs N`。编译 jobs 缺省取非空的 `CMAKE_BUILD_PARALLEL_LEVEL`,再缺省取 CPU/内存预算(开发 Mac 8 路、Lima 2 路),每次打印 `build jobs: …`。
- 测试并行与编译并行分开。`test-fast.sh` 默认 `--test-jobs 4`,可退回 1;`build.sh` 与 CI 显式 `-j 1`,不继承 `CTEST_PARALLEL_LEVEL`。
- `test-fast.sh` 不是完整验证。阶段完成，或改了公共头、跨模块、网络、协议、存储行为时，跑 `./scripts/build.sh`。
- P6 重新冻结后的 R0/最终完整合集为 649/667 项，Unit 为 496/504 项，旧项无删除；扩展合集的计时不代表严格同合集验收。双平台配对、恢复检查与完整回退证据见[阶段报告](../docs/research/build-optimization-results.md#p6双平台最终复测与有界后续决策129)。
- `test-fast.sh` 的退出码:0 通过(有跳过时另行说明，跳过的用例不算验证);1 配置、构建失败或缺二进制，此时不运行旧二进制;2 用例失败或范围内无用例;64 用法错误或非 Unit 目标。
- CTest 的 `-L` 是子串匹配的正则，手工筛选要加锚点:不加锚点时，`-L unit` 会命中名字里含 unit 的身份标签，`-L target=foo` 会命中 `target=foo_bar`。

## 前置条件

- **etcd 二进制**:跨进程集成用例(`new_chain_flow_test`、`realm_journey_test`、`loadgen_integration_test`、`DevServicesScriptTest.*`)会各自拉起一个**真实单节点 etcd** —— 网关的准入消费存储是线性一致存储,attach 路径真实依赖它(ADR-0009),用假 CAS 替身去证明外部系统契约无法归因缺陷。先跑 `./scripts/install-etcd.sh`(安装到 `.tools/etcd-v3.6.14/`,可用 `REALMMESH_ETCD_BINARY` 覆盖路径);二进制缺失即用例失败并提示安装命令,不静默跳过。用例自带的 etcd 用空闲端口 + 临时数据目录,自起自停,因此**不需要**开发机上长期运行 `./scripts/run-etcd-dev.sh`,也不与它抢 2379。
- **MongoDB 二进制**:Player Data 用例(`player_data_store_test`、`login_verify_service_test`、`realm_journey_test`、`loadgen_integration_test`、`DevServicesScriptTest.*`)会拉起**真实单节点副本集 `rs0`** —— 准入事实的 majority 读写与事务只有在真副本集上才有意义(ADR-0011),内存替身只用在不涉及存储契约的单元测试(网关拉取口、Realm 角色存储)。macOS 用 Homebrew 安装 `brew tap mongodb/brew && brew trust mongodb/brew && brew install mongodb-community mongosh`;Linux 跑 `./scripts/install-mongodb.sh`(安装到 `.tools/`,CI 自动执行)。用例永远不连远程共享开发库:改写配置时会删掉 `uri_environment`,shell 里设置了 `REALMMESH_MONGODB_URI` 也不受影响。夹具按 `REALMMESH_MONGOD_BINARY` / `REALMMESH_MONGOSH_BINARY` → PATH → `.tools/` 的顺序查找,缺失即失败并提示安装命令。gtest 二进制内共享一个 mongod、每个用例用独立库名;`DevServicesScriptTest.*` 每条用例自起一个。它们都用空闲端口 + 临时数据目录，自起自停，因此**不需要**运行 Homebrew 服务或 `./scripts/run-mongodb-dev.sh`,也不与它们抢 27017。
- **Python 3**:编译缓存配置/原生契约与 CI 兼容键使用标准库脚本；构建入口不自动安装。macOS 的 Xcode 工具链、Linux 开发环境及 CI 都应提供 `python3`。
- **Ninja 1.11+**:构建图用例(`BuildGraphTest`、`BuildDirScriptTest.*`、`TestFastScriptTest.*`)会在临时目录里用 `dev` 预设配置小工程，验证 Ninja 门槛、原生链接池(含测试可执行文件出池)与 libsodium 安装产物(#123),因此即使主构建用 `--preset dev-make` 回退，全量 ctest 仍需要 PATH 上有真实的 `ninja`。macOS `brew install ninja`,Ubuntu `sudo apt-get install ninja-build`;缺失即用例失败，不跳过。
- 快速子集(`test-fast.sh`、`ctest -L '^unit$'`)不拉起任何进程,无此前置条件。
- `DevServicesScriptTest.StopIsReverseOrdered`、`StopDuringProcessChecks`、
  `StopAfterInterruptedWorkerWait` 与 `StopDuringStartup` 分别验证正常逆序停止、
  连续 TERM 压力、wait 再次中断后的真实回收与就绪等待中的停止；
  同时检查 Supervisor 日志没有 trap 解析错误，管理、工作及服务进程退出，PID 与状态文件清理。
  定向压测使用 `ctest --preset dev -j 1 --repeat until-fail:20 -R '^DevServicesScriptTest\.(StopIsReverseOrdered|StopDuringProcessChecks|StopAfterInterruptedWorkerWait|StopDuringStartup)$'`。
- Linux 依赖安装脚本同时支持 x86_64 与 ARM64（aarch64），下载本机架构的固定版本
  并校验 SHA-256；无需模拟运行 x86_64 二进制。MsQuic 与 MongoDB 使用 Ubuntu 24.04
  上游构建，其他发行版版本需本机验证；CI 的 Linux 门槛仍为 Ubuntu 24.04 x86_64。
- **MsQuic**:Linux 必装(`./scripts/install-msquic-dev.sh`),缺失即配置失败。macOS 可选:`brew install libmsquic` 后重新配置即注册 `quic_transport_test`,不装则只走 TLS/TCP、不注册 QUIC 用例(ADR-0012)。CI 的 `macos` job 不装 MsQuic,QUIC 回归以 `linux` job 为准。
- **M3 冒烟规模按平台分档**:`LoadgenIntegrationTest.M3SmokeTenThousandTicketsAndConcurrentPolls` 在 Linux 跑完整规模(1 万取号 + 2000 轮询),是 M3 的门槛;macOS 缩到 1/2(5000 取号 + 1000 轮询),因为 macOS runner 承载不了瞬时上万 TLS 建连(#104)。两档成功率都是 ≥ 99.9%,规模常量 `kM3SmokeScale` 集中定义在用例旁。
  L1 与 M3 的取号响应先于帧尾指标发布；请求完成后最多等待 2 秒再核对原取号指标门槛（#124），不降低机器人数量或成功率，超时仍由原断言失败。

## 基座

- **C++**:Google Test 1.17(CMake FetchContent);注册入口 `realm_add_gtest`,用例发现 `gtest_discover_tests`,工作目录固定在 tests/cpp 构建目录。
- **Lua**:LuaUnit v3.5(FetchContent 固定 tag+SHA,单文件零依赖)+ `realm_lua_cli`(`third_party/lua` 的最小解释器目标);套件以 `os.exit(lu.LuaUnit.run())` 结尾——退出码 = 失败+错误数,0 即通过;样例结构见 `tests/lua/self_test.lua`。
- 测试目标的注册函数只此两个(`realm_add_gtest` / `realm_add_lua_test`),不要绕开它们直接 `add_test`;脚本级集成的 `add_test` 收敛在 `tests/cmake/*.cmake` 里。

## MongoDB 夹具初始化（#119）

完整构建生成 `realmmesh_mongodb_fixture`：使用现有固定版本 C 驱动在独立进程中
初始化真实单成员 `rs0` 并等待 `hello` 同时确认副本集名称与可写 Primary。
使用 `MongodProcess` 的六个集成目标显式依赖它；DevServices 的 CTest 环境传入同一
可执行路径。每例仍使用自己的本地 `mongod` 与隔离数据库，所有数据回读继续经真实
`mongosh`，辅助程序不进入产品二进制，也不争用被测进程唯一的 C++ 驱动实例。

`REALMMESH_TEST_MONGODB_INITIALIZER` 可覆盖测试初始化器；显式路径缺失或执行失败会
让夹具失败并回收 `mongod`，不会静默改用旧路径。无构建路径的独立夹具消费者保留
旧 `mongosh` 初始化。辅助程序的截止时间涵盖端口等待、驱动调用和 Primary 就绪；
`MongodProcess` 将原 30 秒预算的剩余时间传入。手动开发脚本仍使用原初始化流程。

`MongoFixtureTest` 是串行 `integration` 契约，使用真实本地后端验证初始化、Primary、
初始化失败、超时回收与构造失败后的进程/目录清理。它也计入完整入口性能门槛。

## 原生编译缓存（#124）

配置变量 `REALMMESH_CCACHE=AUTO|ON|OFF` 控制所有原生 C/CXX 目标（含测试与 FetchContent 依赖），默认 AUTO；libsodium 外部 Make 不缓存。可用 `cmake --preset dev -DREALMMESH_CCACHE=OFF` 配置后运行相同 `./scripts/build.sh`，关闭不会删对象缓存。用户预设覆盖目录、工具与 5GiB 上限；CI 显式 ON／2GiB。配置和 launcher 选择不会改变用例标签、合集或测试并行。缓存的命中/失效/缺失/损坏与淘汰验收及未覆盖组合见[阶段报告](../docs/research/build-optimization-results.md)；本机缓存与 CI 恢复/保存的净成本不能混算。

`CompilerCacheConfigTest` 使用 Python 3 与真实 Ninja，覆盖配置、关闭和 CI 兼容键；不要求本机安装 ccache。有效 ccache ≥4.8 可用时另注册 `CompilerCacheNativeTest`，即使主构建 OFF 也运行同一套原生缓存对照；显式工具路径会传入夹具。无有效工具时配置日志明确说明未注册，不能称为缓存行为验收通过。Native 测试验证源码/头/宏/flags/编译器内容/生成头、损坏回退、淘汰后重编、错误传播与实际测量统计目录；使用隔离目录和真实可执行结果。

CI 的可选手动参数 `ccache_acceptance=true` 在同一 runner 上完成三组 OFF/远端热 ON 空完整产物配对；首次填充、OFF 与热 ON 分别跑同一完整串行 CTest，Linux 保留 QUIC 与 M1–M4。性能计时含检查/恢复/保存，正确性测试计时另列；原始证据、失败与维护快照清理流程见 [build-bench](../tools/build-bench/README.md)。普通 PR 不发布共享缓存。判定 CLI 的自测为 `CompilerCacheCISummaryTest`，单独运行 `python3 tests/scripts/ci_cache_summary_test.py`，不能代替 C++ 全集或真实 CI 配对。

Native 夹具按实际编译输入（两个源码、普通头与存在时的生成头）的最新 mtime/ctime
计算剩余安全等待，保持原有 1.05 秒余量；配置已经消耗的时间计入文件年龄，未改输入的
重新配置不重复完整等待。夹具不改文件时间、不放宽 sloppiness，每例仍独立配置目录与
缓存；冷/热及生成头重建另外断言两个编译请求的 direct hit。[#119 后续评估](../docs/research/build-fixture-waits-119.md)
记录局部收益与完整入口门槛的区别。
