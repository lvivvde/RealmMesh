realm_add_gtest(loadgen_core_test
    SOURCES loadgen_core_test.cpp
    LIBS realm_loadgen_core
)

realm_add_gtest(loadgen_login_chain_test
    SOURCES login_chain_adapter_test.cpp
    LIBS realm_loadgen_core
)

realm_add_gtest(metrics_scrape_test
    SOURCES metrics_scrape_test.cpp
    LIBS realm_loadgen_core
)

# L1 三向定向 + M2/M3 缩减档(#48):真实 MeshHost + 真实 TLS 端口 +
# 并发机器人,整目标集成档串行,不进快速子集。
realm_add_gtest(loadgen_integration_test
    SOURCES loadgen_integration_test.cpp
    LIBS
        realm_loadgen_core
        realm_service_host
        httplib::httplib
        nlohmann_json::nlohmann_json
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    DEFINITIONS
        REALMMESH_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE
)
