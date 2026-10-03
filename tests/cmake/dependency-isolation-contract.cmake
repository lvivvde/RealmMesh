# 第三方依赖隔离的配置期契约(#121):OpenSSL 来源校验、protoc 外部入口与共享缓存键
# 隔离都在无依赖的 CMake 模块里，这里逐个分支断言;缓存隔离要真实的 CMakeCache,
# 用例会拉起子 cmake 配置小工程。
add_test(
    NAME DependencyIsolationTest
    COMMAND "${CMAKE_COMMAND}"
        "-DREALM_MESH_TEST_CMAKE_COMMAND=${CMAKE_COMMAND}"
        "-DREALM_MESH_TEST_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DREALM_MESH_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}/dependency-isolation"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/dependency_isolation_test.cmake"
)
set_tests_properties(DependencyIsolationTest PROPERTIES LABELS integration)
