#include "realmmesh/network/transport/transport_factory.hpp"

#include <gtest/gtest.h>

#include <vector>

namespace realm::network {
namespace {

TEST(TransportProtocolTest, UsesStablePublicWireNamesAndValues) {
    EXPECT_EQ(static_cast<std::uint8_t>(TransportProtocol::Quic), 1U);
    EXPECT_EQ(static_cast<std::uint8_t>(TransportProtocol::TlsTcp), 2U);
    EXPECT_EQ(to_string(TransportProtocol::Quic), "quic");
    EXPECT_EQ(to_string(TransportProtocol::TlsTcp), "tls_tcp");
}

TEST(TransportFactoryTest, CreatesOnlyEnabledSecureTransports) {
    const std::vector<TransportConfig> configs{
        {
            .name = "client_tls_tcp",
            .protocol = TransportProtocol::TlsTcp,
            .listen_address = "127.0.0.1",
            .listen_port = 0,
            .tls =
                TransportConfig::TlsServerIdentity{
                    .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
                    .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
                },
        },
        {
            .name = "client_quic",
            .protocol = TransportProtocol::Quic,
            .enabled = false,
            .listen_address = "127.0.0.1",
            .listen_port = 0,
        },
    };
    auto transports = TransportFactory::create_enabled(configs);
    ASSERT_EQ(transports.size(), 1U);
    EXPECT_EQ(transports.front()->name(), "client_tls_tcp");
    EXPECT_EQ(transports.front()->protocol(), TransportProtocol::TlsTcp);
    EXPECT_NE(transports.front()->local_endpoint().port, 0U);
}

TEST(TransportFactoryTest, RejectsDuplicateEnabledNames) {
    const auto identity = TransportConfig::TlsServerIdentity{
        .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
        .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
    };
    const std::vector<TransportConfig> configs{
        {
            .name = "client",
            .protocol = TransportProtocol::TlsTcp,
            .listen_address = "127.0.0.1",
            .tls = identity,
        },
        {
            .name = "client",
            .protocol = TransportProtocol::Quic,
            .listen_address = "127.0.0.1",
            .tls = identity,
        },
    };
    EXPECT_THROW(
        static_cast<void>(TransportFactory::create_enabled(configs)),
        std::invalid_argument);
}

TEST(TransportFactoryTest, RejectsSecureTransportWithoutServerIdentity) {
    const std::vector<TransportConfig> configs{{
        .name = "client_tls",
        .protocol = TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
    }};
    EXPECT_THROW(
        static_cast<void>(TransportFactory::create_enabled(configs)),
        std::invalid_argument);
}

TEST(TransportFactoryTest, RejectsEmptyTlsAlpn) {
    const std::vector<TransportConfig> configs{{
        .name = "client_tls",
        .protocol = TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .tls =
            TransportConfig::TlsServerIdentity{
                .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
                .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
                .alpn = "",
            },
    }};
    EXPECT_THROW(
        static_cast<void>(TransportFactory::create_enabled(configs)),
        std::invalid_argument);
}

}  // namespace
}  // namespace realm::network
