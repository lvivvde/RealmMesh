realm_add_gtest(tls_tcp_transport_test
    SOURCES tls_tcp_transport_test.cpp
    LIBS
        nlohmann_json::nlohmann_json
        realm_network
        RealmMesh::Observability
    DEFINITIONS
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_tls_identity
)

realm_add_gtest(tls_mutual_auth_test
    SOURCES tls_mutual_auth_test.cpp
    LIBS realm_network
    DEFINITIONS
        REALMMESH_TEST_SERVICE_TLS_DIRECTORY="${REALMMESH_TEST_SERVICE_TLS_DIRECTORY}"
        REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
        REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
    DEPENDS realmmesh_test_service_identities realmmesh_test_tls_identity
    LABELS integration
    DISCOVER_PROPERTIES TIMEOUT 10)
