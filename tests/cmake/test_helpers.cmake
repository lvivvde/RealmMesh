# 测试目标注册的唯一入口:统一 GTest 链接、cxx_std_20、用例发现与工作目录。
# 每个测试目录的 CMakeLists 只需一条 realm_add_gtest 调用。
#
#   realm_add_gtest(<name>
#       SOURCES <src> ...
#       [LIBS <lib> ...]
#       [INCLUDE_DIRS <dir> ...]
#       [DEFINITIONS <K=V> ...]
#       [DEPENDS <target> ...]
#       [LABELS <label> ...]                        # 缺省 unit;约定见下
#       [DISCOVER_PROPERTIES <prop> <value> ...]   # 例如 RUN_SERIAL TRUE
#   )
#
# 标签约定(ctest -L 依赖它划分快速子集与全量):unit = 进程内 GTest,
# 不拉子进程、不占固定端口;integration = 驱动真实二进制/真实端口或跨进程
# (e2e、bash 脚本、RUN_SERIAL 性质)。分类标签只有这两个;Lua 套件
# (realm_add_lua_test)在 unit 之外另带 lua 标签,只用于 ctest -L lua 单筛。
# 新增测试必须带标签:常规 GTest 用缺省即可,占用固定端口或拉起
# realm_mesh 的目标必须显式 LABELS integration,否则会混进 ctest -L unit
# 的快速子集里抢端口。快速子集 ctest -L unit,全量 ctest --preset dev。
#
# 用例的工作目录固定在 tests/cpp 构建目录(REALMMESH_TEST_WORKING_DIRECTORY)。
# 拆分前该目录是 gtest_discover_tests 的隐式默认值;现在显式传入,避免依赖
# add_subdirectory 的默认值变化导致测试的相对路径语义漂移。

include(GoogleTest)

function(realm_add_gtest name)
    cmake_parse_arguments(PARSE_ARGV 1 ARG ""
        ""
        "SOURCES;LIBS;INCLUDE_DIRS;DEFINITIONS;DEPENDS;LABELS;DISCOVER_PROPERTIES")

    if(NOT ARG_LABELS)
        set(ARG_LABELS unit)
    endif()

    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE GTest::gtest_main ${ARG_LIBS})
    target_compile_features(${name} PRIVATE cxx_std_20)

    if(ARG_INCLUDE_DIRS)
        target_include_directories(${name} PRIVATE ${ARG_INCLUDE_DIRS})
    endif()
    if(ARG_DEFINITIONS)
        target_compile_definitions(${name} PRIVATE ${ARG_DEFINITIONS})
    endif()
    if(ARG_DEPENDS)
        add_dependencies(${name} ${ARG_DEPENDS})
    endif()

    if(ARG_DISCOVER_PROPERTIES)
        gtest_discover_tests(${name}
            WORKING_DIRECTORY "${REALMMESH_TEST_WORKING_DIRECTORY}"
            PROPERTIES LABELS ${ARG_LABELS} ${ARG_DISCOVER_PROPERTIES}
        )
    else()
        gtest_discover_tests(${name}
            WORKING_DIRECTORY "${REALMMESH_TEST_WORKING_DIRECTORY}"
            PROPERTIES LABELS ${ARG_LABELS}
        )
    endif()
endfunction()

# Lua 测试套件的注册入口:一套件一 add_test;套件文件以 os.exit(lu.LuaUnit.run())
# 结尾,退出码 = 失败+错误数(0 即通过),正是 add_test 的成败约定。
# luaunit 单文件源码由根 CMakeLists 的 FetchContent 提供,经 LUA_PATH 注入
# require 路径(转义分号是 Lua 的「追加默认路径」语法)。
#
#   realm_add_lua_test(<name> <script 绝对路径>
#       [LABELS <label> ...]   # 缺省 unit lua:进快速子集,亦可单独 -L lua 筛选
#   )
#
# 用例的工作目录是调用处的二进制目录,套件里的相对路径以此为基准。
function(realm_add_lua_test name script)
    cmake_parse_arguments(PARSE_ARGV 2 ARG "" "" "LABELS")

    if(NOT ARG_LABELS)
        set(ARG_LABELS unit lua)
    endif()

    add_test(NAME ${name}
        COMMAND "$<TARGET_FILE:realm_lua_cli>" "${script}")
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "LUA_PATH=${luaunit_SOURCE_DIR}/?.lua\;\;"
        LABELS "${ARG_LABELS}")
endfunction()
