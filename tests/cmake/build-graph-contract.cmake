# Ninja 构建图的配置期契约(#123):Ninja 版本门槛与缺失提示、链接池、libsodium 安装产物
# 清单。版本判断与清单校验是无依赖的 CMake 函数/脚本，逐个分支断言;提示与链接池要真实
# 配置，用例会拉起子 cmake 配置小工程(链接池分支需要 PATH 上的真实 ninja);Ninja 构建下
# 另查本工程 build.ninja 里库、realm_mesh 与 GTest 可执行文件的池归属。
add_test(
    NAME BuildGraphTest
    COMMAND "${CMAKE_COMMAND}"
        "-DREALM_MESH_TEST_CMAKE_COMMAND=${CMAKE_COMMAND}"
        "-DREALM_MESH_TEST_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DREALM_MESH_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/build-graph"
        "-DREALM_MESH_TEST_BINARY_DIR=${CMAKE_BINARY_DIR}"
        "-DREALM_MESH_TEST_GENERATOR=${CMAKE_GENERATOR}"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/build_graph_test.cmake"
)
set_tests_properties(BuildGraphTest PROPERTIES LABELS integration)
