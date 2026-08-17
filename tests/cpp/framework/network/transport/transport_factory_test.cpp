#include "realmmesh/network/transport/transport_factory.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace realm::network {
namespace {

KcpSecurityKey kcp_test_key() {
    KcpSecurityKey key{};
    key.front() = std::byte{1};
    return key;
}

TEST(TransportFactoryTest, CreatesOnlyEnabledTransports) {
    const std::vector<TransportConfig> configs{
        {
            .name = "client_tcp",
            .protocol = TransportProtocol::Tcp,
            .listen_address = "127.0.0.1",
            .listen_port = 0,
        },
        {
            .name = "client_udp",
            .protocol = TransportProtocol::Udp,
            .enabled = false,
            .listen_address = "127.0.0.1",
            .listen_port = 9001,
        },
    };

    auto transports = TransportFactory::create_enabled(configs);

    ASSERT_EQ(transports.size(), 1U);
    EXPECT_EQ(transports.front()->name(), "client_tcp");
    EXPECT_EQ(transports.front()->protocol(), TransportProtocol::Tcp);
    const auto endpoint = transports.front()->local_endpoint();
    EXPECT_EQ(endpoint.name, "client_tcp");
    EXPECT_EQ(endpoint.protocol, TransportProtocol::Tcp);
    EXPECT_EQ(endpoint.address, "127.0.0.1");
    EXPECT_NE(endpoint.port, 0U);
}

TEST(TransportFactoryTest, RejectsDuplicateEnabledNames) {
    const std::vector<TransportConfig> configs{
        {.name = "client", .protocol = TransportProtocol::Tcp},
        {.name = "client", .protocol = TransportProtocol::Udp},
    };

    EXPECT_THROW(
        static_cast<void>(TransportFactory::create_enabled(configs)),
        std::invalid_argument);
}

TEST(TransportFactoryTest, CreatesAnEnabledUdpTransport) {
    const std::vector<TransportConfig> configs{{
        .name = "scene_udp",
        .protocol = TransportProtocol::Udp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
    }};

    auto transports = TransportFactory::create_enabled(configs);

    ASSERT_EQ(transports.size(), 1U);
    EXPECT_EQ(transports.front()->protocol(), TransportProtocol::Udp);
    EXPECT_NE(transports.front()->local_endpoint().port, 0U);
}

TEST(TransportFactoryTest, CreatesAnEnabledKcpTransport) {
    const std::vector<TransportConfig> configs{{
        .name = "scene_kcp",
        .protocol = TransportProtocol::Kcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .kcp_ticket_key = kcp_test_key(),
    }};

    auto transports = TransportFactory::create_enabled(configs);

    ASSERT_EQ(transports.size(), 1U);
    EXPECT_EQ(transports.front()->protocol(), TransportProtocol::Kcp);
    EXPECT_NE(transports.front()->local_endpoint().port, 0U);
}

TEST(TransportFactoryTest, RejectsKcpWithoutATicketKey) {
    const std::vector<TransportConfig> configs{{
        .name = "scene_kcp",
        .protocol = TransportProtocol::Kcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
    }};

    EXPECT_THROW(
        static_cast<void>(TransportFactory::create_enabled(configs)),
        std::invalid_argument);
}

}  // namespace
}  // namespace realm::network
