#include "realmmesh/game/gateway/client_session_registry.hpp"

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

TEST(ClientSessionRegistryTest, SendsOnlyThroughTheChosenPrimaryTransport) {
    FakeTransport primary("client_primary", network::TransportProtocol::Quic);
    primary.add_session(41);
    ClientSessionRegistry registry;
    registry.register_transport(primary);

    const auto client = registry.open_primary("client_primary", 41);
    const auto payload = bytes("combat");

    EXPECT_NE(client, invalid_client_session_id);
    EXPECT_EQ(registry.send(client, payload), SendResult::Sent);
    EXPECT_EQ(primary.sent_sessions, std::vector<network::SessionId>{41});
    ASSERT_EQ(primary.sent_payloads.size(), 1U);
    EXPECT_EQ(primary.sent_payloads.front(), payload);
}

}  // namespace
}  // namespace realm::game::gateway
