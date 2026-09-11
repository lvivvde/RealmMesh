#include "realmmesh/game/gateway/edge_session_table.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace realm::game::gateway {
namespace {

class FakeTransport final : public network::IMessageTransport {
public:
    FakeTransport(std::string name, network::TransportProtocol protocol)
        : name_(std::move(name)),
          protocol_(protocol) {}

    std::string_view name() const noexcept override { return name_; }
    network::TransportProtocol protocol() const noexcept override {
        return protocol_;
    }
    network::TransportEndpoint local_endpoint() const override {
        return {.name = name_, .protocol = protocol_};
    }
    std::size_t session_count() const noexcept override {
        return sessions_.size();
    }
    std::vector<network::TransportEvent> poll_once(
        std::chrono::milliseconds) override {
        return {};
    }
    bool send(network::SessionId session_id, std::span<const std::byte> payload)
        override {
        if (!sessions_.contains(session_id)) {
            return false;
        }
        sent_sessions.push_back(session_id);
        sent_payloads.emplace_back(payload.begin(), payload.end());
        return true;
    }
    bool close(network::SessionId session_id) override {
        return sessions_.erase(session_id) != 0;
    }
    bool reload_credentials() override { return true; }

    void add_session(network::SessionId session_id) {
        sessions_.insert(session_id);
    }

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

TEST(EdgeSessionTableTest, SendsOnlyThroughThePrimaryTransport) {
    FakeTransport primary("client_primary", network::TransportProtocol::Quic);
    primary.add_session(41);
    EdgeSessionTable table;
    table.register_transport(primary);

    const auto session = table.open("client_primary", 41);
    const auto payload = bytes("combat");

    EXPECT_NE(session, invalid_edge_session_id);
    ASSERT_TRUE(table.record(session).has_value());
    EXPECT_FALSE(table.record(session)->established);
    EXPECT_EQ(table.record(session)->primary.protocol,
              network::TransportProtocol::Quic);
    EXPECT_EQ(table.send(session, payload), SendResult::Sent);
    EXPECT_EQ(primary.sent_sessions, std::vector<network::SessionId>{41});
    ASSERT_EQ(primary.sent_payloads.size(), 1U);
    EXPECT_EQ(primary.sent_payloads.front(), payload);
}

TEST(EdgeSessionTableTest, EstablishMovesPendingToEstablished) {
    FakeTransport primary("client_primary", network::TransportProtocol::Quic);
    primary.add_session(41);
    EdgeSessionTable table;
    table.register_transport(primary);

    const auto session = table.open("client_primary", 41);
    EXPECT_TRUE(table.establish(session));
    ASSERT_TRUE(table.record(session).has_value());
    EXPECT_TRUE(table.record(session)->established);
    // 重复迁移与未知句柄都不是 established 迁移。
    EXPECT_FALSE(table.establish(session));
    EXPECT_FALSE(table.establish(EdgeSessionId{999}));
}

TEST(EdgeSessionTableTest, FindResolvesOnlyLiveSessions) {
    FakeTransport primary("client_primary", network::TransportProtocol::Quic);
    primary.add_session(41);
    EdgeSessionTable table;
    table.register_transport(primary);

    const auto session = table.open("client_primary", 41);
    EXPECT_EQ(table.find("client_primary", 41), session);
    EXPECT_EQ(table.find("client_primary", 999), std::nullopt);
    EXPECT_EQ(table.find("other_transport", 41), std::nullopt);

    static_cast<void>(table.close_session(session));
    EXPECT_EQ(table.find("client_primary", 41), std::nullopt);
}

TEST(EdgeSessionTableTest, CloseNotifiesTransportAndRemovesTheRecord) {
    FakeTransport primary("client_primary", network::TransportProtocol::Quic);
    primary.add_session(41);
    EdgeSessionTable table;
    table.register_transport(primary);

    const auto session = table.open("client_primary", 41);
    const auto closed = table.close_session(session);

    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->session_id, session);
    EXPECT_FALSE(closed->established);
    EXPECT_EQ(primary.sent_sessions, std::vector<network::SessionId>{});
    EXPECT_EQ(table.record(session), std::nullopt);
    EXPECT_EQ(table.close_session(session), std::nullopt);
}

TEST(EdgeSessionTableTest, TransportReportedCloseRemovesAndSnapshots) {
    FakeTransport primary("client_primary", network::TransportProtocol::Quic);
    primary.add_session(41);
    EdgeSessionTable table;
    table.register_transport(primary);

    const auto session = table.open("client_primary", 41);
    static_cast<void>(table.establish(session));
    const auto closed = table.on_transport_closed("client_primary", 41);

    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->session_id, session);
    EXPECT_TRUE(closed->established);
    EXPECT_EQ(table.record(session), std::nullopt);
    EXPECT_EQ(table.on_transport_closed("client_primary", 41), std::nullopt);
    EXPECT_EQ(table.on_transport_closed("client_primary", 999), std::nullopt);
    EXPECT_EQ(table.on_transport_closed("other_transport", 41), std::nullopt);
}

TEST(EdgeSessionTableTest, UnknownSessionCommandsNeverTouchTransports) {
    FakeTransport primary("client_primary", network::TransportProtocol::Quic);
    primary.add_session(41);
    EdgeSessionTable table;
    table.register_transport(primary);

    EXPECT_EQ(table.send(invalid_edge_session_id, bytes("x")),
              SendResult::UnknownSession);
    EXPECT_EQ(table.send(EdgeSessionId{999}, bytes("x")),
              SendResult::UnknownSession);
    EXPECT_EQ(table.close_session(EdgeSessionId{999}), std::nullopt);
    EXPECT_EQ(primary.sent_sessions, std::vector<network::SessionId>{});
}

}  // namespace
}  // namespace realm::game::gateway
