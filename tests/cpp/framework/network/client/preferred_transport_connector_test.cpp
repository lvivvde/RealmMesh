#include "realmmesh/network/client/preferred_transport_connector.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <algorithm>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <vector>

namespace realm::network::client {
namespace {

class FakeConnection final : public ISecureConnection {
public:
    explicit FakeConnection(TransportProtocol protocol) : protocol_(protocol) {}
    TransportProtocol protocol() const noexcept override { return protocol_; }
private:
    TransportProtocol protocol_;
};

struct Behavior {
    std::chrono::milliseconds delay;
    ConnectFailure failure;
    bool succeeds;
};

class FakeDialer final : public ITransportDialer {
public:
    ConnectAttempt connect(
        const EndpointCandidate& endpoint,
        std::chrono::milliseconds,
        std::stop_token stop_token) override {
        {
            std::lock_guard lock(mutex_);
            calls.push_back(endpoint.protocol);
        }
        const auto behavior = behaviors.at(endpoint.protocol);
        const auto deadline = std::chrono::steady_clock::now() + behavior.delay;
        while (std::chrono::steady_clock::now() < deadline) {
            if (stop_token.stop_requested()) return ConnectFailure::Cancelled;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (behavior.succeeds) {
            return std::make_shared<FakeConnection>(endpoint.protocol);
        }
        return behavior.failure;
    }

    std::unordered_map<TransportProtocol, Behavior> behaviors;
    std::mutex mutex_;
    std::vector<TransportProtocol> calls;
};

const std::vector<EndpointCandidate> candidates{
    {
        .protocol = TransportProtocol::Quic,
        .host = "edge.example.test",
        .port = 8000,
        .priority = 0,
    },
    {
        .protocol = TransportProtocol::TlsTcp,
        .host = "edge.example.test",
        .port = 8000,
        .priority = 1,
    },
};

ConnectorOptions fast_options() {
    return {
        .tls_tcp_delay = std::chrono::milliseconds(20),
        .handshake_timeout = std::chrono::milliseconds(100),
        .round_timeout = std::chrono::milliseconds(200),
        .quic_negative_cache_ttl = std::chrono::seconds(5),
    };
}

TEST(PreferredTransportConnectorTest, ReturnsQuicWithoutStartingTlsWhenFast) {
    FakeDialer dialer;
    dialer.behaviors = {
        {TransportProtocol::Quic,
         {std::chrono::milliseconds(2), ConnectFailure::ProtocolError, true}},
        {TransportProtocol::TlsTcp,
         {std::chrono::milliseconds(2), ConnectFailure::ProtocolError, true}},
    };
    PreferredTransportConnector connector(dialer, fast_options());

    const auto result = connector.connect(candidates, "wifi-a");

    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<ISecureConnection>>(result));
    EXPECT_EQ(
        std::get<std::shared_ptr<ISecureConnection>>(result)->protocol(),
        TransportProtocol::Quic);
    EXPECT_EQ(dialer.calls, std::vector{TransportProtocol::Quic});
}

TEST(PreferredTransportConnectorTest, StartsTlsAfterDelayAndFirstSecureWins) {
    FakeDialer dialer;
    dialer.behaviors = {
        {TransportProtocol::Quic,
         {std::chrono::milliseconds(100), ConnectFailure::HandshakeTimeout, false}},
        {TransportProtocol::TlsTcp,
         {std::chrono::milliseconds(3), ConnectFailure::ProtocolError, true}},
    };
    PreferredTransportConnector connector(dialer, fast_options());

    const auto result = connector.connect(candidates, "wifi-a");

    ASSERT_TRUE(std::holds_alternative<std::shared_ptr<ISecureConnection>>(result));
    EXPECT_EQ(
        std::get<std::shared_ptr<ISecureConnection>>(result)->protocol(),
        TransportProtocol::TlsTcp);
    ASSERT_EQ(dialer.calls.size(), 2U);
}

TEST(PreferredTransportConnectorTest, DoesNotFallbackOnCertificateFailure) {
    FakeDialer dialer;
    dialer.behaviors = {
        {TransportProtocol::Quic,
         {std::chrono::milliseconds(2), ConnectFailure::CertificateRejected, false}},
        {TransportProtocol::TlsTcp,
         {std::chrono::milliseconds(2), ConnectFailure::ProtocolError, true}},
    };
    PreferredTransportConnector connector(dialer, fast_options());

    EXPECT_EQ(
        std::get<ConnectFailure>(connector.connect(candidates, "wifi-a")),
        ConnectFailure::CertificateRejected);
    EXPECT_EQ(dialer.calls, std::vector{TransportProtocol::Quic});
}

TEST(PreferredTransportConnectorTest, CachesNetworkQuicFailureUntilNetworkChanges) {
    FakeDialer dialer;
    dialer.behaviors = {
        {TransportProtocol::Quic,
         {std::chrono::milliseconds(2), ConnectFailure::NetworkUnreachable, false}},
        {TransportProtocol::TlsTcp,
         {std::chrono::milliseconds(2), ConnectFailure::ProtocolError, true}},
    };
    PreferredTransportConnector connector(dialer, fast_options());

    static_cast<void>(connector.connect(candidates, "wifi-a"));
    static_cast<void>(connector.connect(candidates, "wifi-a"));
    connector.network_changed();
    static_cast<void>(connector.connect(candidates, "wifi-b"));

    EXPECT_EQ(
        std::ranges::count(dialer.calls, TransportProtocol::Quic),
        2);
    EXPECT_EQ(
        std::ranges::count(dialer.calls, TransportProtocol::TlsTcp),
        3);
}

}  // namespace
}  // namespace realm::network::client
