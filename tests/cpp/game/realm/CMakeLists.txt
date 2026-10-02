realm_add_gtest(character_name_test
    SOURCES character_name_test.cpp
    LIBS realm_game_realm
)

realm_add_gtest(training_rule_test
    SOURCES training_rule_test.cpp
    LIBS realm_game_realm
    DEFINITIONS REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

realm_add_gtest(realm_sessions_test
    SOURCES realm_sessions_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS realm_game_realm
    DEFINITIONS REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
)

realm_add_gtest(realm_journey_test
    SOURCES realm_journey_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS
        nlohmann_json::nlohmann_json
        realm_client
        realm_cluster
        realm_game_common
    DEFINITIONS
        REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_MESH_EXECUTABLE="$<TARGET_FILE:realm_mesh>"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realm_mesh realmmesh_test_tls_identity
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 120
)
