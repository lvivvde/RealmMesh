#pragma once

#include "realmmesh/concurrency/bounded_queue.hpp"
#include "realmmesh/game/gateway/gateway_server.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace realm::game::gateway {

enum class QueueResult : std::uint8_t {
    Queued,
    Full,
    Stopped,
};

struct GatewayRuntimeStats {
    std::uint64_t overload_disconnects{0};
    std::uint64_t rejected_outbound_commands{0};
    std::uint64_t successful_deliveries{0};
    std::uint64_t failed_deliveries{0};
};

class GatewayRuntime final {
public:
    explicit GatewayRuntime(GatewayConfig config);
    explicit GatewayRuntime(
        GatewayConfig config,
        GatewayRuntimeOptions options);
    ~GatewayRuntime();

    GatewayRuntime(const GatewayRuntime&) = delete;
    GatewayRuntime& operator=(const GatewayRuntime&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::optional<network::TransportEndpoint> local_endpoint(
        std::string_view transport_name) const;
    [[nodiscard]] const std::vector<network::TransportEndpoint>& local_endpoints()
        const noexcept;

    [[nodiscard]] std::optional<GatewayEvent> try_receive();
    [[nodiscard]] std::vector<GatewayEvent> drain_events(std::size_t max_events);

    [[nodiscard]] QueueResult try_send(
        ClientSessionId client_session_id,
        std::span<const std::byte> payload);
    [[nodiscard]] QueueResult try_send_channel(
        std::string_view transport_name,
        network::SessionId transport_session_id,
        std::span<const std::byte> payload);
    [[nodiscard]] QueueResult try_accept_connection(
        std::string_view transport_name,
        network::SessionId transport_session_id,
        std::span<const std::byte> response);
    [[nodiscard]] QueueResult try_close_channel(
        std::string_view transport_name,
        network::SessionId transport_session_id);
    [[nodiscard]] QueueResult try_reload_credentials();

    [[nodiscard]] GatewayRuntimeStats stats() const noexcept;
    [[nodiscard]] std::optional<std::string> terminal_error() const;

private:
    enum class CommandKind : std::uint8_t {
        SendClient,
        SendChannel,
        AcceptConnection,
        CloseChannel,
        ReloadCredentials,
    };

    struct OutboundCommand {
        CommandKind kind;
        ClientSessionId client_session_id{invalid_client_session_id};
        std::string transport_name;
        network::SessionId transport_session_id{network::invalid_session_id};
        std::vector<std::byte> payload;
    };

    [[nodiscard]] QueueResult enqueue(OutboundCommand command);
    void io_loop(std::stop_token stop_token) noexcept;
    void process_outbound_commands();
    void process_command(OutboundCommand command);
    void publish_event(GatewayEvent event);

    GatewayServer server_;
    GatewayRuntimeOptions options_;
    std::uint16_t local_port_{0};
    std::vector<network::TransportEndpoint> local_endpoints_;
    concurrency::BoundedQueue<GatewayEvent> inbound_;
    concurrency::BoundedQueue<OutboundCommand> outbound_;
    std::jthread io_thread_;
    std::atomic_bool running_{false};
    std::atomic_uint64_t overload_disconnects_{0};
    std::atomic_uint64_t rejected_outbound_commands_{0};
    std::atomic_uint64_t successful_deliveries_{0};
    std::atomic_uint64_t failed_deliveries_{0};
    mutable std::mutex terminal_error_mutex_;
    std::optional<std::string> terminal_error_;
};

}  // namespace realm::game::gateway
