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
        REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    # 每个用例进程自带一个真实 mongod 副本集(ADR-0011),启动加选举要数秒。
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 30
)
