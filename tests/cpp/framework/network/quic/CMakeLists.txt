# QUIC is Linux-only (ADR-0002); its tests exist only where MsQuic was found.
if(REALM_MESH_MSQUIC_FOUND)
    realm_add_gtest(quic_transport_test
        SOURCES quic_transport_test.cpp
        LIBS
            nlohmann_json::nlohmann_json
            realm_network
            RealmMesh::Observability
        DEFINITIONS
            REALMMESH_TEST_TLS_CERTIFICATE="${REALMMESH_TEST_TLS_CERTIFICATE}"
            REALMMESH_TEST_TLS_PRIVATE_KEY="${REALMMESH_TEST_TLS_PRIVATE_KEY}"
        DEPENDS realmmesh_test_tls_identity
    )
endif()
