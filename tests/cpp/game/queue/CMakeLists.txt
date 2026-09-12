realm_add_gtest(queue_number_test
    SOURCES queue_number_test.cpp
    LIBS realm_queue_service
)

realm_add_gtest(queue_handler_test
    SOURCES queue_handler_test.cpp
    LIBS realm_queue_service
)

realm_add_gtest(queue_core_test
    SOURCES queue_core_test.cpp
    LIBS realm_queue_service
)

realm_add_gtest(queue_store_test
    SOURCES queue_store_test.cpp
    LIBS
        nlohmann_json::nlohmann_json
        realm_queue_service
)

# 真实 TLS loopback 占内核临时端口,见 tests/README.md:不进快速子集。
realm_add_gtest(queue_service_test
    SOURCES queue_service_test.cpp
    LIBS realm_queue_service
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    DEFINITIONS
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
)
