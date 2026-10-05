realm_add_gtest(gateway_server_test
    SOURCES gateway_server_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    # realm_service_host 公开依赖 realm_gateway_service,后者提供
    # GatewayConfigLoader::load(只返回配置值,不带 Lua 头);只列前者可避免
    # 同一静态库在链接行重复出现。
    LIBS realm_service_host
    DEFINITIONS
        REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
)

realm_add_gtest(gateway_runtime_test
    SOURCES gateway_runtime_test.cpp
    LIBS realm_gateway_service
    DEFINITIONS
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    # 真实 TLS 回环传输会绑定端口,按 tests/README.md 归 integration。
    LABELS integration
)

realm_add_gtest(edge_session_table_test
    SOURCES edge_session_table_test.cpp
    LIBS realm_gateway_service
)

realm_add_gtest(gateway_primary_transport_test
    SOURCES gateway_primary_transport_test.cpp
    LIBS realm_gateway_service
)

# Gateway 轻量头不经传递 include 引入 gateway_runtime.hpp(#128):编译即断言。
realm_add_gtest(gateway_headers_test
    SOURCES gateway_headers_test.cpp
    LIBS realm_gateway_service
)

realm_add_gtest(account_fetch_port_test
    SOURCES account_fetch_port_test.cpp
    LIBS realm_gateway_service
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
)

realm_add_gtest(admission_consumption_store_test
    SOURCES admission_consumption_store_test.cpp
    LIBS realm_gateway_service nlohmann_json::nlohmann_json
)

realm_add_gtest(gateway_admission_test
    SOURCES gateway_admission_test.cpp
    LIBS realm_gateway_service
)

# 直接驱动 Lua 解析入口:显式链接 realm_scripting(#126)。
realm_add_gtest(gateway_login_config_test
    SOURCES gateway_login_config_test.cpp
    LIBS realm_gateway_service realm_scripting
)

realm_add_gtest(gateway_ingress_test
    SOURCES gateway_ingress_test.cpp
    LIBS realm_gateway_service
)

realm_add_gtest(gateway_login_pipeline_test
    SOURCES gateway_login_pipeline_test.cpp
    LIBS realm_gateway_service
)

# 入口守卫的限流契约:进程内、不占端口,进快速子集。
realm_add_gtest(gateway_ingress_throttle_test
    SOURCES gateway_ingress_throttle_test.cpp
    LIBS realm_gateway_service
)
