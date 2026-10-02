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

- 分类标签只有两个:`unit` / `integration`,Lua 套件另带的 `lua` 只作筛选;三者定义都在 `test_helpers.cmake` 头注释与根目录 `CONTEXT.md`(Testing 词目)。
- 新增测试必须带标签:常规 GTest / Lua 套件用缺省即可;**占用固定端口或拉起 `realm_mesh` 的目标必须显式 `LABELS integration`**,否则会混进快速子集抢端口。
- 配置脚本不在 Lua 侧测:真实加载路径在 C++(`LuaRuntime` 沙箱 + `LayeredConfigLoader`),由 `configs_load_smoke_test` 覆盖;Lua 测试的火力留给业务逻辑模块(地图 #15 / 调研票 #16 的边界结论)。

## 运行方式

| 场景 | 命令 |
|---|---|
| TDD 反馈环(保存即重跑快速子集) | `./scripts/test-watch.sh`(单跑一轮加 `--once`) |
| 手动快速子集 | `ctest --preset dev -L unit`(或 `-L lua` 只筛 Lua 用例) |
| 一键全量(构建 + 全部测试) | `./scripts/build.sh` |
| 仅全量测试 | `ctest --preset dev` |
| macOS 登录链验收 | `./scripts/run-macos-login-acceptance.sh`（TLS/TCP：本仓客户端没有 QUIC 拨号器，报告另记 Gateway 的 QUIC 监听是否编入；成功链默认重复 3 次，并在 `build/dev/acceptance/` 生成报告和原始日志） |
| Linux M1–M4 登录链验收 | `./scripts/run-linux-login-acceptance.sh`（Linux QUIC + TLS/TCP，默认重复关键组 3 次，并在 `build/dev/acceptance/` 生成报告和原始日志） |
| 全量兜底 | push / PR 时 GitHub Actions 在 macOS + Linux 双平台跑 `ctest --preset dev`(`.github/workflows/ci.yml`) |

## 前置条件

- **etcd 二进制**:跨进程集成用例(`new_chain_flow_test`、`realm_journey_test`、`loadgen_integration_test`、`DevServicesScriptTest.*`)会各自拉起一个**真实单节点 etcd** —— 网关的准入消费存储是线性一致存储,attach 路径真实依赖它(ADR-0009),用假 CAS 替身去证明外部系统契约无法归因缺陷。先跑 `./scripts/install-etcd.sh`(安装到 `.tools/etcd-v3.6.14/`,可用 `REALMMESH_ETCD_BINARY` 覆盖路径);二进制缺失即用例失败并提示安装命令,不静默跳过。用例自带的 etcd 用空闲端口 + 临时数据目录,自起自停,因此**不需要**开发机上长期运行 `./scripts/run-etcd-dev.sh`,也不与它抢 2379。
- **MongoDB 二进制**:Player Data 用例(`player_data_store_test`、`login_verify_service_test`、`realm_journey_test`、`loadgen_integration_test`、`DevServicesScriptTest.*`)会拉起**真实单节点副本集 `rs0`** —— 准入事实的 majority 读写与事务只有在真副本集上才有意义(ADR-0011),内存替身只用在不涉及存储契约的单元测试(网关拉取口、Realm 角色存储)。macOS 用 Homebrew 安装 `brew tap mongodb/brew && brew trust mongodb/brew && brew install mongodb-community mongosh`;Linux 跑 `./scripts/install-mongodb.sh`(安装到 `.tools/`,CI 自动执行)。用例永远不连远程共享开发库:改写配置时会删掉 `uri_environment`,shell 里设置了 `REALMMESH_MONGODB_URI` 也不受影响。夹具按 `REALMMESH_MONGOD_BINARY` / `REALMMESH_MONGOSH_BINARY` → PATH → `.tools/` 的顺序查找,缺失即失败并提示安装命令。gtest 二进制内共享一个 mongod、每个用例用独立库名;`DevServicesScriptTest.*` 每条用例自起一个。它们都用空闲端口 + 临时数据目录，自起自停，因此**不需要**运行 Homebrew 服务或 `./scripts/run-mongodb-dev.sh`,也不与它们抢 27017。
- 快速子集 `ctest -L unit` 不拉起任何进程,无此前置条件。
- Linux 依赖安装脚本同时支持 x86_64 与 ARM64（aarch64），下载本机架构的固定版本
  并校验 SHA-256；无需模拟运行 x86_64 二进制。MsQuic 与 MongoDB 使用 Ubuntu 24.04
  上游构建，其他发行版版本需本机验证；CI 的 Linux 门槛仍为 Ubuntu 24.04 x86_64。
- **MsQuic**:Linux 必装(`./scripts/install-msquic-dev.sh`),缺失即配置失败。macOS 可选:`brew install libmsquic` 后重新配置即注册 `quic_transport_test`,不装则只走 TLS/TCP、不注册 QUIC 用例(ADR-0012)。CI 的 `macos` job 不装 MsQuic,QUIC 回归以 `linux` job 为准。
- **M3 冒烟规模按平台分档**:`LoadgenIntegrationTest.M3SmokeTenThousandTicketsAndConcurrentPolls` 在 Linux 跑完整规模(1 万取号 + 2000 轮询),是 M3 的门槛;macOS 缩到 1/2(5000 取号 + 1000 轮询),因为 macOS runner 承载不了瞬时上万 TLS 建连(#104)。两档成功率都是 ≥ 99.9%,规模常量 `kM3SmokeScale` 集中定义在用例旁。

## 基座

- **C++**:Google Test 1.17(CMake FetchContent);注册入口 `realm_add_gtest`,用例发现 `gtest_discover_tests`,工作目录固定在 tests/cpp 构建目录。
- **Lua**:LuaUnit v3.5(FetchContent 固定 tag+SHA,单文件零依赖)+ `realm_lua_cli`(`third_party/lua` 的最小解释器目标);套件以 `os.exit(lu.LuaUnit.run())` 结尾——退出码 = 失败+错误数,0 即通过;样例结构见 `tests/lua/self_test.lua`。
- 测试目标的注册函数只此两个(`realm_add_gtest` / `realm_add_lua_test`),不要绕开它们直接 `add_test`;脚本级集成的 `add_test` 收敛在 `tests/cmake/*.cmake` 里。
