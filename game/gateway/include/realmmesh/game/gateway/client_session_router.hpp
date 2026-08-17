#pragma once

#include "realmmesh/network/transport/message_transport.hpp"

#include <cstdint>
#include <array>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realm::game::gateway {

using ClientSessionId = std::uint64_t;
inline constexpr ClientSessionId invalid_client_session_id = 0;

enum class FallbackPolicy : std::uint8_t {
    UseTcp,
    DropIfUnavailable,
};

struct SendOptions {
    network::TransportProtocol preferred{network::TransportProtocol::Tcp};
    FallbackPolicy fallback{FallbackPolicy::UseTcp};
};

enum class SendResult : std::uint8_t {
    SentPreferred,
    SentViaTcpFallback,
    ClientNotFound,
    ChannelUnavailable,
    SendFailed,
};

struct ClientChannel {
    std::string transport_name;
    network::TransportProtocol protocol{network::TransportProtocol::Tcp};
    network::SessionId transport_session_id{network::invalid_session_id};

    bool operator==(const ClientChannel&) const = default;
};

class ClientSessionRouter final {
public:
    void register_transport(network::IMessageTransport& transport);

    [[nodiscard]] ClientSessionId open_tcp_session(
        std::string_view transport_name,
        network::SessionId transport_session_id);
    [[nodiscard]] bool bind_channel(
        ClientSessionId client_session_id,
        std::string_view transport_name,
        network::SessionId transport_session_id);
    void close_channel(
        std::string_view transport_name,
        network::SessionId transport_session_id);

    [[nodiscard]] std::optional<ClientSessionId> find_client(
        std::string_view transport_name,
        network::SessionId transport_session_id) const;
    [[nodiscard]] std::optional<ClientChannel> channel(
        ClientSessionId client_session_id,
        network::TransportProtocol protocol) const;
    [[nodiscard]] std::vector<ClientSessionId> client_ids() const;
    [[nodiscard]] std::size_t client_count() const noexcept;

    [[nodiscard]] SendResult send(
        ClientSessionId client_session_id,
        std::span<const std::byte> payload,
        SendOptions options = {});

private:
    struct ClientSession {
        ClientSessionId id;
        std::array<std::optional<ClientChannel>, 3> channels;
    };

    struct ChannelKey {
        std::string transport_name;
        network::SessionId transport_session_id;

        bool operator==(const ChannelKey&) const = default;
    };

    struct ChannelKeyHash {
        std::size_t operator()(const ChannelKey& key) const noexcept {
            const auto name_hash = std::hash<std::string>{}(key.transport_name);
            const auto session_hash =
                std::hash<network::SessionId>{}(key.transport_session_id);
            return name_hash ^ (session_hash + 0x9e3779b9U +
                                (name_hash << 6U) + (name_hash >> 2U));
        }
    };

    [[nodiscard]] network::IMessageTransport* find_transport(
        std::string_view name) const;
    void erase_client(ClientSessionId client_session_id);

    ClientSessionId next_client_session_id_{1};
    std::unordered_map<std::string, network::IMessageTransport*> transports_;
    std::unordered_map<ClientSessionId, ClientSession> clients_;
    std::unordered_map<ChannelKey, ClientSessionId, ChannelKeyHash> clients_by_channel_;
};

}  // namespace realm::game::gateway
