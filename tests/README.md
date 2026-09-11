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

- 标签只有两个:`unit` / `integration`,定义见 `test_helpers.cmake` 头注释与根目录 `CONTEXT.md`(Testing 词目)。
- 新增测试必须带标签:常规 GTest / Lua 套件用缺省即可;**占用固定端口或拉起 `realm_mesh` 的目标必须显式 `LABELS integration`**,否则会混进快速子集抢端口。
- 配置脚本不在 Lua 侧测:真实加载路径在 C++(`LuaRuntime` 沙箱 + `LayeredConfigLoader`),由 `configs_load_smoke_test` 覆盖;Lua 测试的火力留给业务逻辑模块(地图 #15 / 调研票 #16 的边界结论)。

## 运行方式

| 场景 | 命令 |
|---|---|
| TDD 反馈环(保存即重跑快速子集) | `./scripts/test-watch.sh`(单跑一轮加 `--once`) |
| 手动快速子集 | `ctest --preset dev -L unit`(或 `-L lua` 只筛 Lua 用例) |
| 一键全量(构建 + 全部测试) | `./scripts/build.sh` |
| 仅全量测试 | `ctest --preset dev` |
| 全量兜底 | push / PR 时 GitHub Actions 在 macOS + Linux 双平台跑 `ctest --preset dev`(`.github/workflows/ci.yml`) |

## 基座

- **C++**:Google Test 1.17(CMake FetchContent);注册入口 `realm_add_gtest`,用例发现 `gtest_discover_tests`,工作目录固定在 tests/cpp 构建目录。
- **Lua**:LuaUnit v3.5(FetchContent 固定 tag+SHA,单文件零依赖)+ `realm_lua_cli`(`third_party/lua` 的最小解释器目标);套件以 `os.exit(lu.LuaUnit.run())` 结尾——退出码 = 失败+错误数,0 即通过;样例结构见 `tests/lua/self_test.lua`。
- 测试目标的注册函数只此两个(`realm_add_gtest` / `realm_add_lua_test`),不要绕开它们直接 `add_test`;脚本级集成的 `add_test` 收敛在 `tests/cmake/*.cmake` 里。
