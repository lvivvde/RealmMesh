realm_add_gtest(account_store_test
    SOURCES account_store_test.cpp
    LIBS realm_game_common
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
)

realm_add_gtest(base64url_test
    SOURCES base64url_test.cpp
    LIBS realm_game_common
)

realm_add_gtest(compact_jws_test
    SOURCES compact_jws_test.cpp
    LIBS realm_game_common
)

realm_add_gtest(edge_attach_protocol_test
    SOURCES edge_attach_protocol_test.cpp
    LIBS realm_game_common
)

realm_add_gtest(identity_replay_guard_test
    SOURCES identity_replay_guard_test.cpp
    LIBS realm_game_common
)

realm_add_gtest(identity_token_test
    SOURCES identity_token_test.cpp
    LIBS realm_game_common
)

realm_add_gtest(json_test
    SOURCES json_test.cpp
    LIBS realm_game_common
)

realm_add_gtest(queue_number_test
    SOURCES queue_number_test.cpp
    LIBS realm_game_common
)

realm_add_gtest(session_ticket_test
    SOURCES session_ticket_test.cpp
    LIBS realm_game_common
)

realm_add_gtest(three_stage_flow_test
    SOURCES three_stage_flow_test.cpp
    LIBS
        nlohmann_json::nlohmann_json
        realm_game_common
        realm_network
    DEFINITIONS
        REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_MESH_EXECUTABLE="$<TARGET_FILE:realm_mesh>"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS
        realm_mesh
        realmmesh_test_tls_identity
    # 拉起真实 realm_mesh 三阶段流程,属集成测试:不进快速子集,见 test_helpers.cmake。
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 15
)
