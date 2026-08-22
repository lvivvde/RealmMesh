#pragma once

#include "realmmesh/network/transport/message_transport.hpp"

#include <cstddef>
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

enum class SendResult : std::uint8_t {
    Sent,
    ClientNotFound,
    SendFailed,
};

struct PrimaryTransport {
    std::string transport_name;
    network::TransportProtocol protocol{network::TransportProtocol::TlsTcp};
    network::SessionId transport_session_id{network::invalid_session_id};

    bool operator==(const PrimaryTransport&) const = default;
};

class ClientSessionRegistry final {
public:
    void register_transport(network::IMessageTransport& transport);

    [[nodiscard]] ClientSessionId open_primary(
        std::string_view transport_name,
        network::SessionId transport_session_id);
    [[nodiscard]] std::optional<ClientSessionId> close_primary(
        std::string_view transport_name,
        network::SessionId transport_session_id);

    [[nodiscard]] std::optional<ClientSessionId> find_client(
        std::string_view transport_name,
        network::SessionId transport_session_id) const;
    [[nodiscard]] std::optional<PrimaryTransport> primary(
        ClientSessionId client_session_id) const;
    [[nodiscard]] std::vector<ClientSessionId> client_ids() const;
    [[nodiscard]] std::size_t client_count() const noexcept;

    [[nodiscard]] SendResult send(
        ClientSessionId client_session_id,
        std::span<const std::byte> payload);

private:
    struct PrimaryKey {
        std::string transport_name;
        network::SessionId transport_session_id;

        bool operator==(const PrimaryKey&) const = default;
    };

    struct PrimaryKeyHash {
        std::size_t operator()(const PrimaryKey& key) const noexcept {
            const auto name_hash = std::hash<std::string>{}(key.transport_name);
            const auto session_hash =
                std::hash<network::SessionId>{}(key.transport_session_id);
            return name_hash ^ (session_hash + 0x9e3779b9U +
                                (name_hash << 6U) + (name_hash >> 2U));
        }
    };

    [[nodiscard]] network::IMessageTransport* find_transport(
        std::string_view name) const;

    ClientSessionId next_client_session_id_{1};
    std::unordered_map<std::string, network::IMessageTransport*> transports_;
    std::unordered_map<ClientSessionId, PrimaryTransport> primaries_;
    std::unordered_map<PrimaryKey, ClientSessionId, PrimaryKeyHash>
        clients_by_primary_;
};

}  // namespace realm::game::gateway
