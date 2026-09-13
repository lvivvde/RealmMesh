realm_add_gtest(service_host_test
    SOURCES
        layered_config_loader_test.cpp
        mesh_host_e2e_test.cpp
        mesh_host_test.cpp
        mode2_test.cpp
        observability_artifacts_test.cpp
        service_frame_edge_budget_test.cpp
        service_host_test.cpp
        startup_topology_test.cpp
        ${PROJECT_SOURCE_DIR}/tests/cpp/support/src/fake_service_registry.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS
        httplib::httplib
        nlohmann_json::nlohmann_json
        realm_service_host
    DEFINITIONS
        REALMMESH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    # E2E/Mode2 用例占用真实固定端口且依赖 setenv,须与其他测试串行。
    # 整目标属集成测试:不进快速子集,见 test_helpers.cmake。
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE
)

# 真实 configs/ 树的生产加载冒烟:不占端口、不起进程,留在快速子集。
realm_add_gtest(configs_load_smoke_test
    SOURCES configs_load_smoke_test.cpp
    LIBS realm_service_host
    DEFINITIONS
        REALMMESH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
)
