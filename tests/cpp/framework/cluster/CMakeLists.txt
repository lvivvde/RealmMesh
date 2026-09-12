realm_add_gtest(service_registry_test
    SOURCES
        budget_publisher_test.cpp
        etcd_service_registry_test.cpp
        service_registry_test.cpp
        ${PROJECT_SOURCE_DIR}/tests/cpp/support/src/fake_service_registry.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS
        realm_cluster
        nlohmann_json::nlohmann_json
)
