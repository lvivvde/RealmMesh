#include "realmmesh/game/gateway/client_session_router.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace realm::game::gateway {
namespace {

class FakeTransport final : public network::IMessageTransport {
public:
    FakeTransport(std::string name, network::TransportProtocol protocol)
        : name_(std::move(name)), protocol_(protocol) {}

    std::string_view name() const noexcept override { return name_; }
    network::TransportProtocol protocol() const noexcept override { return protocol_; }
    network::TransportEndpoint local_endpoint() const override {
        return {.name = name_, .protocol = protocol_};
    }
    std::size_t session_count() const noexcept override { return sessions_.size(); }
    std::vector<network::TransportEvent> poll_once(
        std::chrono::milliseconds) override { return {}; }
    bool send(
        network::SessionId session_id,
        std::span<const std::byte> payload) override {
        if (fail_send || !sessions_.contains(session_id)) {
            return false;
        }
        sent_sessions.push_back(session_id);
        sent_payloads.emplace_back(payload.begin(), payload.end());
        return true;
    }
    bool close(network::SessionId session_id) override {
        return sessions_.erase(session_id) != 0;
    }

    void add_session(network::SessionId session_id) { sessions_.insert(session_id); }

    bool fail_send{false};
    std::vector<network::SessionId> sent_sessions;
    std::vector<std::vector<std::byte>> sent_payloads;

private:
    std::string name_;
    network::TransportProtocol protocol_;
    std::unordered_set<network::SessionId> sessions_;
};

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    std::ranges::transform(text, std::back_inserter(result), [](char value) {
        return static_cast<std::byte>(value);
    });
    return result;
}

TEST(ClientSessionRouterTest, UsesPreferredBoundChannel) {
    FakeTransport tcp("client_tcp", network::TransportProtocol::Tcp);
    FakeTransport udp("client_udp", network::TransportProtocol::Udp);
    tcp.add_session(11);
    udp.add_session(22);
    ClientSessionRouter router;
    router.register_transport(tcp);
    router.register_transport(udp);
    const auto client = router.open_tcp_session("client_tcp", 11);
    ASSERT_TRUE(router.bind_channel(client, "client_udp", 22));

    const auto payload = bytes("position");
    const auto result = router.send(
        client,
        payload,
        {.preferred = network::TransportProtocol::Udp});

    EXPECT_EQ(result, SendResult::SentPreferred);
    EXPECT_TRUE(tcp.sent_payloads.empty());
    ASSERT_EQ(udp.sent_payloads.size(), 1U);
    EXPECT_EQ(udp.sent_payloads.front(), payload);
}

TEST(ClientSessionRouterTest, FallsBackToTcpWhenPreferredChannelIsMissing) {
    FakeTransport tcp("client_tcp", network::TransportProtocol::Tcp);
    tcp.add_session(11);
    ClientSessionRouter router;
    router.register_transport(tcp);
    const auto client = router.open_tcp_session("client_tcp", 11);

    const auto result = router.send(
        client,
        bytes("position"),
        {.preferred = network::TransportProtocol::Udp});

    EXPECT_EQ(result, SendResult::SentViaTcpFallback);
    EXPECT_EQ(tcp.sent_sessions, std::vector<network::SessionId>{11});
}

TEST(ClientSessionRouterTest, CanDropWhenPreferredChannelIsMissing) {
    FakeTransport tcp("client_tcp", network::TransportProtocol::Tcp);
    tcp.add_session(11);
    ClientSessionRouter router;
    router.register_transport(tcp);
    const auto client = router.open_tcp_session("client_tcp", 11);

    const auto result = router.send(
        client,
        bytes("snapshot"),
        {
            .preferred = network::TransportProtocol::Udp,
            .fallback = FallbackPolicy::DropIfUnavailable,
        });

    EXPECT_EQ(result, SendResult::ChannelUnavailable);
    EXPECT_TRUE(tcp.sent_payloads.empty());
}

TEST(ClientSessionRouterTest, FallsBackWhenPreferredTransportSendFails) {
    FakeTransport tcp("client_tcp", network::TransportProtocol::Tcp);
    FakeTransport kcp("client_kcp", network::TransportProtocol::Kcp);
    tcp.add_session(11);
    kcp.add_session(33);
    kcp.fail_send = true;
    ClientSessionRouter router;
    router.register_transport(tcp);
    router.register_transport(kcp);
    const auto client = router.open_tcp_session("client_tcp", 11);
    ASSERT_TRUE(router.bind_channel(client, "client_kcp", 33));

    const auto result = router.send(
        client,
        bytes("combat"),
        {.preferred = network::TransportProtocol::Kcp});

    EXPECT_EQ(result, SendResult::SentViaTcpFallback);
    EXPECT_EQ(tcp.sent_sessions, std::vector<network::SessionId>{11});
}

TEST(ClientSessionRouterTest, ClosingTcpRemovesTheLogicalClient) {
    FakeTransport tcp("client_tcp", network::TransportProtocol::Tcp);
    FakeTransport udp("client_udp", network::TransportProtocol::Udp);
    tcp.add_session(11);
    udp.add_session(22);
    ClientSessionRouter router;
    router.register_transport(tcp);
    router.register_transport(udp);
    const auto client = router.open_tcp_session("client_tcp", 11);
    ASSERT_TRUE(router.bind_channel(client, "client_udp", 22));

    router.close_channel("client_tcp", 11);

    EXPECT_EQ(router.client_count(), 0U);
    EXPECT_FALSE(router.find_client("client_udp", 22).has_value());
}

}  // namespace
}  // namespace realm::game::gateway
