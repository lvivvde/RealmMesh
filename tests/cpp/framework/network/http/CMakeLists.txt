realm_add_gtest(http1_codec_test
    SOURCES http1_codec_test.cpp
    LIBS realm_network
)

# 真实 TLS loopback 占临时端口,见 tests/README.md:不进快速子集。
realm_add_gtest(http_server_test
    SOURCES http_server_test.cpp
    LIBS realm_network
    DEFINITIONS
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    LABELS integration
)
