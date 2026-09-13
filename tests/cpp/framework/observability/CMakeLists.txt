realm_add_gtest(observability_logger_test
    SOURCES logger_test.cpp
    LIBS
        realm_observability
        httplib::httplib
        nlohmann_json::nlohmann_json
)

realm_add_gtest(observability_metrics_registry_test
    SOURCES metrics_registry_test.cpp
    LIBS realm_observability
)
