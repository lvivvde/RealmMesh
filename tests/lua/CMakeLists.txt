# 纯 Lua 测试套件(luaunit 经 realm_add_lua_test 注册)。
# 目录约定:业务 Lua 模块出现后,其套件按模块放这里的子目录;
# 配置脚本(configs/*.lua)的加载冒烟不在这里——真实加载路径在 C++ 侧,
# 由 tests/cpp/framework/service_host/configs_load_smoke_test.cpp 覆盖。

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/test_helpers.cmake)

realm_add_lua_test(LuaHarnessSelfTest
    "${CMAKE_CURRENT_SOURCE_DIR}/self_test.lua")
