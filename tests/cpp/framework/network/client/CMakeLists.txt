realm_add_gtest(preferred_transport_connector_test
    SOURCES preferred_transport_connector_test.cpp
    LIBS realm_network
)

realm_add_gtest(json_field_test
    SOURCES json_field_test.cpp
    LIBS realm_network
)

# QUIC 候选即时判 Unsupported、TLS/TCP 拒连判 NetworkUnreachable:
# 纯拨号语义,不占端口 → unit(缺省)。
realm_add_gtest(tls_tcp_client_dialer_test
    SOURCES tls_tcp_client_dialer_test.cpp
    LIBS realm_network
)
