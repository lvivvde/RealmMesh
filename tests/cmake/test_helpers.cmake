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
# (e2e、bash 脚本、RUN_SERIAL 性质)。新增测试必须带标签:常规 GTest 用
# 缺省即可,占用固定端口或拉起 realm_mesh 的目标必须显式 LABELS integration,
# 否则会混进 ctest -L unit 的快速子集里抢端口。快速子集 ctest -L unit,
# 全量 ctest --preset dev。
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
