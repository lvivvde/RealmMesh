# 调研：纯 Lua 单元测试框架选型与 CTest 接入

- 日期：2026-09-12
- 票据：[lvivvde/RealmMesh#16](https://github.com/lvivvde/RealmMesh/issues/16)（地图 [#15](https://github.com/lvivvde/RealmMesh/issues/15)）
- 状态：已定稿（推荐结论见 [TL;DR](#tldr)）
- 方法：对照一手来源（上游仓库 README/源码/rockspec/gh api releases 元数据、本仓库 CMake 与 Lua 5.4.8 官方源码）逐条核实；所有引用见文末。

## TL;DR

**推荐 [LuaUnit](https://github.com/bluebird75/luaunit) v3.5（tag `LUAUNIT_V3_5`，2026-03-26 发布）单文件接入，配合用 FetchContent 的 Lua 源码补一个最小解释器可执行目标（`src/lua.c` + `realm_lua` 静态库），每个测试套件一个 `add_test`，以「套件以 `os.exit(lu.LuaUnit.run())` 结尾 → 退出码 = 失败+错误数（0 即通过）」作为 CTest 的通过判定。**

关键理由：

1. **真·单文件零依赖**：上游 README 明确 "LuaUnit is contained into a single-file and has no external dependency"；对 `luaunit.lua` v3.5 源码的核查也证实全部 `require` 只有 `require("math")`（标准库，从 `package.loaded` 命中，无文件系统查找）。放一个文件进构建树即可跑，完全匹配仓库「第三方仅走 CMake FetchContent」（ADR-0003 精简树）的惯例。
2. **Lua 5.4 支持成熟、维护活跃**：v3.4（2021-03-04）加入 Lua 5.4 支持，v3.5（2026-03-26）新增 5.5 支持；上游自述测试套件在 Lua 5.1–5.5 与 LuaJIT 2.0 上全量回归。能正确处理 5.4 的整数/浮点二分语义（`assertEquals(1, 1.0)` 类断言按 5.4 类型规则处理）。
3. **退出码/输出格式天然适配 CTest**：`LuaUnit.run()`/`runSuite()` 返回「失败+错误数」（源码 `notSuccessCount = failureCount + errorCount`），0 即成功，直接满足 `add_test` 的非零即失败约定；同时原生提供 text/TAP/junit 三种输出，失败信息带 文件:行号 + 期望/实际差异 + 回溯。

对比之下：**busted** 虽然活跃（v2.3.0，2026-01-07）但运行时依赖 8 个 LuaRocks 包（penlight、luassert、say、lua-term、mediator_lua、lua_cliargs、luasystem、dkjson），多文件发行、无单文件 vendor 路径，与 FetchContent-only 惯例冲突；**u-test / minctest-lua / lunatest** 均已停更（2019/2017/2010 年代中期）且未声明 5.4 支持；**tarantool/luatest** 活跃但绑定 Tarantool 生态。

---

## 背景与仓库事实（已核实）

- `third_party/lua/CMakeLists.txt`：Lua 5.4.8 官方 tarball 经 FetchContent（URL + `URL_HASH` SHA256 固定），仅编译静态库 `realm_lua`（`src/*.c` 全量**除** `lua.c`/`luac.c`），平台宏 Linux=`LUA_USE_LINUX`、Apple=`LUA_USE_POSIX`（均为 `PRIVATE`），`dl`/`m` 链接为 `PUBLIC`，`include` 目录 `SYSTEM PUBLIC`。**没有解释器可执行目标。**
- C++ 测试：GTest 1.17 经 FetchContent，`tests/cmake/test_helpers.cmake` 的 `realm_add_gtest` 是唯一注册入口，`gtest_discover_tests` + 固定 `WORKING_DIRECTORY`（`REALMMESH_TEST_WORKING_DIRECTORY` = tests/cpp 构建目录）。
- CI：GitHub Actions，macOS + Linux 双平台 `cmake --preset dev` / `build` / `ctest --preset dev`；无 Windows。
- 配置加载：真实加载路径全在 C++ 侧 —— `LuaRuntime`（sol2；只开 base/coroutine/table/string/math/utf8 六个库，且 `dofile`/`loadfile` 置 nil 的沙箱）+ `LayeredConfigLoader`（configs/common/*.lua 深合并 services/<name>.lua）。已有 `tests/cpp/framework/service_host/layered_config_loader_test.cpp`（合成临时目录 fixture，未扫真实 `configs/` 树）与 `tests/cpp/framework/scripting/lua_runtime_test.cpp`。
- ADR-0003 删除了 `tests/lua` 空占位目录——现在按「有代码才建目录」原则新建 `tests/lua/` 是符合该 ADR 的（有真实测试文件才建）。

## 问题 1：候选框架对比

| 框架 | 最新版本（日期） | Lua 5.4 | 依赖面 / 发行形态 | 维护状态 | 结论 |
|---|---|---|---|---|---|
| **luaunit**（[bluebird75/luaunit](https://github.com/bluebird75/luaunit)） | v3.5（2026-03-26） | ✅ v3.4 起支持（2021-03-04），v3.5 加 5.5；官方在 5.1–5.5 + LuaJIT 2.0 全量回归 | 单文件 `luaunit.lua`，零外部依赖（源码核查仅 `require("math")`）；LuaRocks 可选，drop-in 即用 | 活跃（1.2k+ commits，自述 99.5% 测试覆盖，9 个 open issue）；BSD 许可 | **推荐** |
| **busted**（[Olivine-Labs/busted](https://github.com/Olivine-Labs/busted)，现 lunarmodules） | v2.3.0（2026-01-07） | ✅ "Supports Lua >= 5.1, luajit >= 2.0.0" | 多文件 + **8 个 LuaRocks 运行时依赖**（rockspec：lua_cliargs、luasystem、dkjson、say、luassert、lua-term、penlight、mediator_lua）；README 唯一非 rocks 路径是 Docker 镜像 | 活跃 | 否——依赖面与 FetchContent-only 惯例冲突 |
| [u-test](https://github.com/IUdalov/u-test) | 无 release；最后 push 2019-01-22 | ❌ README 仅声明 5.1/5.2/5.3 + LuaJIT | 单文件 | 停更（2019） | 否 |
| [minctest-lua](https://github.com/codeplea/minctest-lua) | 最后 push 2017-04-22 | 未声明 | 极简 assert 集 | 停更（2017） | 否 |
| lunatest（见 [lua-users wiki](http://lua-users.org/wiki/UnitTesting)） | 2010 年代中期后无活动 | 未声明 | 单文件 | 停更 | 否 |
| [tarantool/luatest](https://github.com/tarantool/luatest) | 活跃（2026-07 仍有 push） | 未独立声明 | LuaRocks 生态、面向 Tarantool 运行时 | 活跃但生态绑定 | 否——脱离 Tarantool 无意义 |

说明：busted 的输出为 pretty/plain/JSON/TAP（README 未提 JUnit，JUnit 需第三方 reporter）；luaunit 原生 text/TAP/junit。功能面上 busted 的 BDD 风格（describe/it）对业务 Lua 更「时髦」，但为此引入 8 包依赖树 + LuaRocks 工具链不符合本仓库的精简树取向。

## 问题 2：零依赖 vendor 的可行性

- **luaunit：可行且是真单文件。** 证据链：
  - README："For simplicity, LuaUnit is contained into a single-file and has no external dependency."
  - 仓库根目录核查：运行时文件只有 `luaunit.lua`；根下的 `junitxml/` 目录只是 JUnit 兼容性文档（XSD schema、Jenkins/Bamboo/Ant 示例 XML、Java formatter 参考），不含任何 Lua 运行时代码。
  - v3.5 `luaunit.lua` 源码核查：唯一的 `require` 是 `require("math")`——标准库模块，`require` 先查 `package.loaded`（`luaL_openlibs` 已放入），不触发文件系统查找，任何标准 Lua 解释器下都成立。
- **busted：不可行。** 多文件发行 + rockspec 声明的 8 个运行时依赖；想 vendor 等于拖整条 LuaRocks 依赖树进仓库。
- **接入方式（保持 FetchContent 惯例）**：不直接把 `luaunit.lua` 提交进 git，而是按 `third_party/lua` 的既有模式在 `BUILD_TESTING` 块内 `FetchContent_Declare(luaunit URL https://github.com/bluebird75/luaunit/archive/refs/tags/LUAUNIT_V3_5.tar.gz URL_HASH SHA256=<实现时计算并固定> DOWNLOAD_EXTRACT_TIMESTAMP TRUE)` + `FetchContent_Populate`（沿用仓库里 CMP0169 的 Populate 写法），测试进程经 `LUA_PATH` 环境变量把 `${luaunit_SOURCE_DIR}` 加进 `package.path`（见问题 4 的 sketch）。URL+SHA 固定与 lua/GTest 的既有写法一致，升级 = 改两行。

## 问题 3：用 FetchContent 源码编译最小 lua 解释器（`lua.c` + `realm_lua`）有没有坑

**总体：没有实质坑，是低风险改动。** 基于 Lua 5.4.8 官方源码（本仓库 `_deps/lua_source-src` 即同一份）逐点核查：

1. **符号冲突：无。** `realm_lua` 的源列表本来就不含 `lua.c`/`luac.c`，`main` 只存在于新增的可执行目标里。注意只编 `src/lua.c`，**不要**把 `luac.c` 也加进来（它自带 `main`）。
2. **平台宏：不传染，需自给（但不给也能跑）。** `realm_lua` 上的 `LUA_USE_LINUX`/`LUA_USE_POSIX` 是 `PRIVATE`，不会传给解释器目标。解释器目标建议镜像同样的按平台定义以获得 `sigaction`/`isatty` 行为；即使完全不给宏，`lua.c` 也有 ISO-C 兜底路径（`signal()` + 「假定 stdin 是 tty」），`lua -e`/脚本执行在 CI 里不受影响——所以这是「一致性」问题而不是「正确性」问题。
3. **readline：没有依赖。** `luaconf.h` 中 `LUA_USE_LINUX` 只联动 `LUA_USE_POSIX` + `LUA_USE_DLOPEN`（5.4.8 luaconf.h 第 61–66 行）；`LUA_USE_READLINE` 不由 luaconf.h 定义，是官方 Makefile 显式加的 `-DLUA_USE_READLINE`。本仓库不定义它，`lua.c` 走 `fgets` 兜底——不会引入 libreadline/ncurses 依赖。
4. **标准库：齐全。** `luaL_openlibs` 来自 `linit.c`，已在 `realm_lua` 源列表中，解释器能拿到全部标准库。
5. **链接依赖：自动继承。** `dl`/`m` 与 include 目录都是 `realm_lua` 的 `PUBLIC` 依赖，解释器目标只要 `target_link_libraries(realm_lua_cli PRIVATE realm_lua)` 即可。
6. **Windows：自动成立。** `_WIN32` 下 `luaconf.h` 自动定义 `LUA_USE_WINDOWS`；且 CI 只有 macOS + Linux。
7. **命名建议**：目标名用 `realm_lua_cli`（或 `realm_lua_interpreter`），避免与任何系统 `lua` 概念混淆；如需二进制名可加 `set_target_properties(... OUTPUT_NAME lua)`。

Sketch（追加到 `third_party/lua/CMakeLists.txt` 尾部）：

```cmake
add_executable(realm_lua_cli ${lua_source_SOURCE_DIR}/src/lua.c)
target_link_libraries(realm_lua_cli PRIVATE realm_lua)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_compile_definitions(realm_lua_cli PRIVATE LUA_USE_LINUX)
elseif(APPLE)
    target_compile_definitions(realm_lua_cli PRIVATE LUA_USE_POSIX)
endif()
```

## 问题 4：CTest 集成推荐模式

**约定：每个 Lua 测试套件 = 一个 `add_test`；套件文件以 `os.exit(lu.LuaUnit.run())` 结尾。**

- **退出码约定（一手来源核实）**：`LuaUnit.run()` / `runner:runSuite()` 返回值即退出码——"It returns the number of failures and errors. On success 0 is returned, making is suitable for an exit code."（参考文档原文）。源码对应：`notSuccessCount = failureCount + errorCount`（`luaunit.lua` L3063），`runSuite` 末尾 `return self.result.notSuccessCount`（L2296）。非零即失败，正好是 `add_test` 的原生语义。两个特殊退出码：junit 输出缺 `-n/--name` 时 `os.exit(-1)`；`--error/--failure` 中止模式触发时 `os.exit(-2)`。若想绝对稳妥（>255 个失败时 `os.exit` 的状态字节会截断），可写 `os.exit(lu.LuaUnit.run() == 0 and 0 or 1)`。
- **输出格式**：`-o/--output text|tap|junit|nil`（或 `LUAUNIT_OUTPUT` 环境变量）。推荐**默认 text 起步**（`ctest --output-on-failure` 下可读性最好：失败用例名 + 断言 文件：行号 + 期望/实际差异表 + 回溯）；要逐用例一行（CI 日志 grep 友好）可 `--output TAP`；**junit 模式必须同时给 `-n <文件路径>`**——XML 写文件、stdout 只打进度（缺 `-n` 直接 `os.exit(-1)`），将来接 CI 报表时再开。
- **防作弊细节**：luaunit 在测试运行期间重写 `os.exit`，业务代码里误调 `os.exit(0)` 不会伪装成功（源码 L130–143 明确为此设计）——对把业务模块直接 `dofile` 进测试的场景是隐性加分。
- **过滤与粒度**：`-p PATTERN`/`-x PATTERN` 按 Lua pattern 包含/排除用例，`-v/-q` 控制详细度；起步用「一套件一 add_test」的粒度（与 `tests/scripts` 的 bash 一脚本一测试粒度对齐），不学 `gtest_discover_tests` 的逐用例发现。
- **注册入口**：照 `realm_add_gtest` 的样子在 `tests/cmake/test_helpers.cmake` 加一个 `realm_add_lua_test`，保持「唯一注册入口」惯例：

```cmake
# tests/cmake/test_helpers.cmake 追加
function(realm_add_lua_test name script)
    add_test(NAME ${name}
        COMMAND $<TARGET_FILE:realm_lua_cli> ${script})
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${REALMMESH_TEST_WORKING_DIRECTORY}"
        ENVIRONMENT "LUA_PATH=${luaunit_SOURCE_DIR}/?.lua;;"
        LABELS "lua")
endfunction()
```

  要点：`$<TARGET_FILE:...>` 生成器表达式免掉解释器路径拼接；`ENVIRONMENT` 里的尾部 `;;` 是 Lua 的「保留默认路径」语法，追加自定义路径不破坏默认 `package.path`；`WORKING_DIRECTORY` 与 GTest 用例保持同一约定。
- **套件写法**：

```lua
-- tests/lua/xxx_test.lua
local lu = require("luaunit")
TestSomething = {}
function TestSomething:setUp() end
function TestSomething:test_example() lu.assertEquals(1 + 1, 2) end

os.exit(lu.LuaUnit.run())
```

- **目录**：新建 `tests/lua/` 放套件（ADR-0003 删除的是空占位，现在有真实测试文件即符合「有代码才建」）。

## 问题 5：configs/*.lua 冒烟测试放 Lua 侧还是 C++ 侧

**结论：不值得在 Lua 侧用同一框架做；继续留在 C++（sol2）侧，必要时扩展现有 GTest 而非新起 Lua 套件。**

理由：

1. **真实加载路径在 C++ 侧**。配置最终由 `LuaRuntime`（sol2，沙箱：只开 6 个标准库、`dofile`/`loadfile` 置 nil）+ `LayeredConfigLoader`（C++ 深合并）消费。Lua 侧用裸解释器做「冒烟」，环境与宿主不一致：多出全套标准库、没有宿主注入的全局、合并语义对不上——冒烟通过不代表服务器能加载，测的是另一个世界。
2. **C++ 侧已有现成 harness 与真实缺口**。`layered_config_loader_test.cpp` 目前用合成临时目录 fixture，**没有**扫真实 `configs/` 树；「所有真实 configs/*.lua + main.config 能被 LuaRuntime 编译、能被 LayeredConfigLoader 加载」这个缺口，在既有 GTest 里加一个遍历 `configs/` 的用例即可补上，且测的就是生产路径。
3. **Lua 侧测试的火力应该留给纯 Lua 业务模块**（问题票背景里「用户已确认未来会写 Lua 业务逻辑」），那才是 luaunit 的主场；configs 冒烟混进去只会稀释目录语义。
4. 地图 #15「Not yet specified」里本就挂着「configs/*.lua 的加载/金样测试边界放 Lua 侧还是 C++ 侧（等 Lua 接入落地后划分）」——本调研的答案是：**加载冒烟归 C++，纯业务逻辑归 Lua**，可作为该条目的预答案。

## 推荐接入方案汇总

1. `third_party/lua/CMakeLists.txt` 追加 `realm_lua_cli` 可执行目标（问题 3 sketch，4 行实质代码）。
2. 根 `CMakeLists.txt` 的 `BUILD_TESTING` 块内按既有 FetchContent 模式引入 luaunit v3.5（tag tarball + `URL_HASH` 固定，实现时计算 SHA256；沿用 CMP0169 Populate 写法）。
3. `tests/cmake/test_helpers.cmake` 追加 `realm_add_lua_test`（问题 4 sketch）。
4. 新建 `tests/lua/`，套件以 `os.exit(lu.LuaUnit.run())` 结尾；起步用默认 text 输出，需要时切 TAP/junit。
5. configs 加载冒烟：在既有 C++ GTest 中补一个遍历真实 `configs/` 树的用例，不建 Lua 侧冒烟。

## 参考链接

- LuaUnit 仓库（单文件/零依赖/输出格式/版本支持声明）：https://github.com/bluebird75/luaunit
- LuaUnit releases（v3.5 = 2026-03-26；v3.4 = 2021-03-04，加 Lua 5.4）：https://github.com/bluebird75/luaunit/releases
- LuaUnit 参考文档（runSuite 退出码语义、-o text/tap/junit/nil、junit 需 -n、-p/-x 过滤）：https://github.com/bluebird75/luaunit/blob/master/doc/4_reference_doc.rst
- LuaUnit v3.5 源码（`require("math")` 唯一依赖；`notSuccessCount = failureCount + errorCount` L3063；`os.exit` 防作弊 L130–143；junit 缺名 `os.exit(-1)` L3541）：https://github.com/bluebird75/luaunit/blob/LUAUNIT_V3_5/luaunit.lua
- LuaUnit 许可（BSD）：https://github.com/bluebird75/luaunit/blob/LUAUNIT_V3_5/LICENSE.txt
- busted 仓库（Lua >= 5.1 / LuaJIT；LuaRocks 安装路径；pretty/plain/JSON/TAP 输出）：https://github.com/Olivine-Labs/busted
- busted rockspec（8 个运行时依赖）：https://github.com/Olivine-Labs/busted/blob/master/busted-scm-1.rockspec
- busted releases（v2.3.0 = 2026-01-07）：https://github.com/Olivine-Labs/busted/releases
- u-test（单文件但 2019 停更、仅声明 5.1–5.3）：https://github.com/IUdalov/u-test
- minctest-lua（2017 停更）：https://github.com/codeplea/minctest-lua
- tarantool/luatest（活跃但 Tarantool 生态绑定）：https://github.com/tarantool/luatest
- lua-users wiki 框架总览：http://lua-users.org/wiki/UnitTesting
- Lua 5.4.8 官方源码（`lua.c` 平台宏与 ISO-C 兜底、`luaconf.h` 的 `LUA_USE_LINUX` 宏链、`_WIN32` 自动 `LUA_USE_WINDOWS`）：https://www.lua.org/ftp/lua-5.4.8.tar.gz（本仓库 FetchContent 缓存 `build/dev/_deps/lua_source-src/` 为同一份）
