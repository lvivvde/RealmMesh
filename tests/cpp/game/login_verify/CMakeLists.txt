realm_add_gtest(login_verify_handler_test
    SOURCES login_verify_handler_test.cpp
    LIBS realm_login_verify_service
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
)

# 真实 TLS loopback 占内核临时端口,见 tests/README.md:不进快速子集。
realm_add_gtest(login_verify_service_test
    SOURCES login_verify_service_test.cpp
    LIBS realm_login_verify_service
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    DEFINITIONS
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
)
