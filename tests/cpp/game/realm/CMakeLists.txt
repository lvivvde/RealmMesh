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
    LIBS realmmesh_test_mongodb
        nlohmann_json::nlohmann_json
        realm_game_client
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

# Realm 业务帧(RealmFrame)直连入场对撞真实 TLS loopback:占临时端口、
# 依赖 setenv,属集成测试，串行跑。
realm_add_gtest(realm_frame_test
    SOURCES
        realm_frame_test.cpp
        ${PROJECT_SOURCE_DIR}/tests/cpp/support/src/fake_service_registry.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS realm_game_realm
        realm_cluster
        nlohmann_json::nlohmann_json
    DEFINITIONS
        REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE
)

realm_add_gtest(realm_scenes_test SOURCES realm_scenes_test.cpp
    LIBS realm_game_realm realm_game_scene realm_network realm_observability
    DEFINITIONS REALMMESH_TEST_SERVICE_TLS_DIRECTORY="${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}"
    DEPENDS realmmesh_test_service_identities
    LABELS integration DISCOVER_PROPERTIES TIMEOUT 15)

realm_add_gtest(realm_chat_test SOURCES realm_chat_test.cpp
    LIBS realm_game_realm realm_game_chat realm_network realm_observability
    DEFINITIONS REALMMESH_TEST_SERVICE_TLS_DIRECTORY="${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}"
    DEPENDS realmmesh_test_service_identities
    LABELS integration DISCOVER_PROPERTIES TIMEOUT 15)

realm_add_gtest(chat_journey_test SOURCES chat_journey_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS realmmesh_test_mongodb nlohmann_json::nlohmann_json httplib::httplib realm_game_client realm_cluster realm_game_common
    DEFINITIONS
        REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_MESH_EXECUTABLE="$<TARGET_FILE:realm_mesh>"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
        REALMMESH_TEST_SERVICE_TLS_DIRECTORY="${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}"
    DEPENDS realm_mesh realmmesh_test_tls_identity realmmesh_test_service_identities
    LABELS integration DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
