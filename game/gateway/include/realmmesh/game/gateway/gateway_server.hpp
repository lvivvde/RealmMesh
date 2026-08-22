#pragma once

#include "realmmesh/game/gateway/client_session_registry.hpp"
#include "realmmesh/cluster/service_discovery_config.hpp"
#include "realmmesh/network/transport/transport_config.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realm::game::gateway {

struct GatewayRuntimeOptions {
    std::size_t inbound_capacity{65'536};
    std::size_t outbound_capacity{65'536};
    std::size_t max_commands_per_cycle{4'096};
    std::chrono::milliseconds io_poll_interval{2};
};

struct GatewayConfig {
    std::vector<network::TransportConfig> transports;
    GatewayRuntimeOptions runtime;
    cluster::ServiceDiscoveryConfig service_discovery;
    observability::LoggerConfig logging;
    observability::MetricsServerConfig logging_metrics;
    observability::ServiceIdentity logging_identity;
    std::uint32_t tick_rate{20};
    std::size_t max_events_per_frame{4'096};
    std::string downstream_address;
    std::uint16_t downstream_port{0};
};

enum class GatewayEventKind : std::uint8_t {
    ConnectionOpened,
    MessageReceived,
    ClientSessionOpened,
    ConnectionClosed,
    PeerAddressChanged,
};

struct GatewayEvent {
    GatewayEventKind kind;
    std::optional<ClientSessionId> client_session_id;
    std::string transport_name;
    network::TransportProtocol protocol{network::TransportProtocol::TlsTcp};
    network::SessionId transport_session_id{network::invalid_session_id};
    std::vector<std::byte> payload;
};

class GatewayServer final {
public:
    explicit GatewayServer(GatewayConfig config);

    GatewayServer(const GatewayServer&) = delete;
    GatewayServer& operator=(const GatewayServer&) = delete;

    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::optional<network::TransportEndpoint> local_endpoint(
        std::string_view transport_name) const;
    [[nodiscard]] std::vector<network::TransportEndpoint> local_endpoints() const;
    [[nodiscard]] std::size_t connection_count() const noexcept;
    [[nodiscard]] std::size_t pending_connection_count() const noexcept;
    [[nodiscard]] std::size_t client_count() const noexcept;
    [[nodiscard]] std::vector<ClientSessionId> client_ids() const;
    [[nodiscard]] std::optional<ClientSessionId> find_client(
        std::string_view transport_name,
        network::SessionId transport_session_id) const;
    [[nodiscard]] std::optional<ClientSessionId> promote_connection(
        std::string_view transport_name,
        network::SessionId transport_session_id);

    [[nodiscard]] SendResult send(
        ClientSessionId client_session_id,
        std::span<const std::byte> payload);
    [[nodiscard]] bool send_channel(
        std::string_view transport_name,
        network::SessionId transport_session_id,
        std::span<const std::byte> payload);
    [[nodiscard]] bool close_channel(
        std::string_view transport_name,
        network::SessionId transport_session_id);
    [[nodiscard]] bool reload_credentials();

    [[nodiscard]] std::vector<GatewayEvent> poll_events(
        std::chrono::milliseconds timeout);
    void poll_once(std::chrono::milliseconds timeout);

private:
    [[nodiscard]] network::IMessageTransport* find_transport(
        std::string_view transport_name) const;

    std::vector<std::unique_ptr<network::IMessageTransport>> transports_;
    ClientSessionRegistry client_registry_;
    std::unordered_map<std::string, std::unordered_set<network::SessionId>>
        pending_connections_;
};

}  // namespace realm::game::gateway
