realm_add_gtest(scene_shard_test SOURCES scene_shard_test.cpp LIBS realm_game_scene)

realm_add_gtest(scene_journey_test SOURCES scene_journey_test.cpp
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    LIBS realm_game_scene realmmesh_test_mongodb nlohmann_json::nlohmann_json httplib::httplib realm_game_client realm_cluster realm_game_common
    DEFINITIONS
        REALMMESH_MESH_EXECUTABLE="$<TARGET_FILE:realm_mesh>"
        REALMMESH_SCENE_CONTROL_EXECUTABLE="$<TARGET_FILE:realm_mesh_scene_control>"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
        REALMMESH_TEST_SERVICE_TLS_DIRECTORY="${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}"
    DEPENDS realm_mesh realm_mesh_scene_control realmmesh_test_tls_identity realmmesh_test_service_identities
    LABELS integration
    # S5 的 257 次真实持久移动验证 256 通知边界；保留规模，为 CI 留出截止余量。
    DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 360)

# 仅故障客户端直接使用 OpenSSL，不向产品或其他测试传播使用需求。
target_link_libraries(scene_journey_test PRIVATE OpenSSL::SSL)
