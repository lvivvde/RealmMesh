realm_add_gtest(edge_runtime_test
    SOURCES edge_runtime_test.cpp
    LIBS realm_edge
    DEFINITIONS
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    # 真实 TLS 回环传输会绑定端口,按 tests/README.md 归 integration。
    LABELS integration
)

realm_add_gtest(edge_connection_table_test
    SOURCES edge_connection_table_test.cpp
    LIBS realm_edge
)

realm_add_gtest(edge_source_address_test
    SOURCES edge_source_address_test.cpp
    LIBS realm_edge
)
