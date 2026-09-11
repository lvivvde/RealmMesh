# REALMMESH_NET_BACKEND 开关的配置期契约:映射逻辑在一个无依赖的 CMake 模块里,
# 因此每个分支都在这里断言。
add_test(
    NAME NetBackendSwitchTest
    COMMAND "${CMAKE_COMMAND}"
        "-DREALM_MESH_TEST_CMAKE_COMMAND=${CMAKE_COMMAND}"
        "-DREALM_MESH_TEST_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
        "-DREALM_MESH_TEST_WORK_DIR=${CMAKE_CURRENT_BINARY_DIR}"
        -P "${PROJECT_SOURCE_DIR}/tests/cmake/net_backend_test.cmake"
)
set_tests_properties(NetBackendSwitchTest PROPERTIES LABELS integration)
