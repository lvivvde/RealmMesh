cmake_minimum_required(VERSION 3.20)

project(
    RealmMesh
    VERSION 0.1.0
    DESCRIPTION "A frame-driven C++ game server"
    LANGUAGES C CXX
)

add_subdirectory(third_party/lua)
add_subdirectory(third_party/sol2)
add_subdirectory(third_party/sodium)
add_subdirectory(third_party/json)
add_subdirectory(third_party/httplib)
add_subdirectory(third_party/msquic)
add_subdirectory(third_party/protobuf)
add_subdirectory(third_party/spdlog)
add_subdirectory(proto)
add_subdirectory(framework/concurrency)
add_subdirectory(framework/observability)
add_subdirectory(framework/scripting)
add_subdirectory(framework/scheduler)
add_subdirectory(framework/service)
add_subdirectory(framework/network)
add_subdirectory(framework/cluster)
add_subdirectory(framework/service_host)
add_subdirectory(game/common)
add_subdirectory(game/gateway)
add_subdirectory(game/login_verify)
add_subdirectory(game/queue)
add_subdirectory(apps/mesh_host)
add_subdirectory(tools/loadgen)
# detach 辅助程序只为缺少 setsid(1) 的平台而建(macOS;见 scripts/lib/dev-process.sh)。
# 其它平台系统自带 setsid,建了也不会被用到。
if(APPLE)
    add_subdirectory(tools/detach)
endif()

include(CTest)
if(BUILD_TESTING)
    include(FetchContent)

    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.tar.gz
        URL_HASH SHA256=65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(googletest)

    # 纯 Lua 测试基座(luaunit,单文件零依赖):只要源码树,不要其构建目标;
    # 套件经 tests/cmake/test_helpers.cmake 的 realm_add_lua_test 注册。
    FetchContent_Declare(
        luaunit
        URL https://github.com/bluebird75/luaunit/archive/refs/tags/LUAUNIT_V3_5.tar.gz
        URL_HASH SHA256=bd0085dc35c847afab0dbafc3669490ddc81b85623068ee0debe7667ffc03454
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    if(POLICY CMP0169)
        cmake_policy(PUSH)
        cmake_policy(SET CMP0169 OLD)
    endif()
    FetchContent_GetProperties(luaunit)
    if(NOT luaunit_POPULATED)
        FetchContent_Populate(luaunit)
    endif()
    if(POLICY CMP0169)
        cmake_policy(POP)
    endif()

    add_subdirectory(tests/cpp)
    add_subdirectory(tests/lua)
endif()
