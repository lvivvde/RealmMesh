realm_add_gtest(scene_shard_test SOURCES scene_shard_test.cpp LIBS realm_game_scene)

realm_add_gtest(scene_journey_test SOURCES scene_journey_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS realmmesh_test_mongodb nlohmann_json::nlohmann_json realm_game_client realm_cluster realm_game_common
    DEFINITIONS
        REALMMESH_MESH_EXECUTABLE="$<TARGET_FILE:realm_mesh>"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
        REALMMESH_TEST_SERVICE_TLS_DIRECTORY="${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}"
    DEPENDS realm_mesh realmmesh_test_tls_identity realmmesh_test_service_identities
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
