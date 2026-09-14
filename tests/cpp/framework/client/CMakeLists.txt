realm_add_gtest(adaptive_poller_test
    SOURCES adaptive_poller_test.cpp
    LIBS realm_client
)

# 七态链路与回退规则:脚本化传输口,不占端口 → unit(缺省)。
realm_add_gtest(login_chain_test
    SOURCES login_chain_test.cpp
    LIBS realm_client
)

# 真实 TLS loopback 上的整条登录链路(HttpStub + 网关桩 + 真实 realm 服务):
# 占临时端口、带时钟等待 → integration,串行跑。
realm_add_gtest(wire_login_transport_integration_test
    SOURCES wire_login_transport_integration_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS
        realm_client
        realm_service_host
        RealmMesh::GameCommon
    DEFINITIONS
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE
)
