realm_add_gtest(chat_channels_test SOURCES chat_channels_test.cpp LIBS realm_game_chat)
realm_add_gtest(chat_service_test SOURCES chat_service_test.cpp
    LIBS realm_game_chat realm_cluster httplib::httplib
    INCLUDE_DIRS ${PROJECT_SOURCE_DIR}/tests/cpp/support/include
    DEFINITIONS
        REALMMESH_MESH_EXECUTABLE="$<TARGET_FILE:realm_mesh>"
        REALMMESH_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        REALMMESH_TEST_SERVICE_TLS_DIRECTORY="${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realm_mesh realmmesh_test_service_identities realmmesh_test_tls_identity
    LABELS integration
    DISCOVER_PROPERTIES RUN_SERIAL TRUE TIMEOUT 30)
