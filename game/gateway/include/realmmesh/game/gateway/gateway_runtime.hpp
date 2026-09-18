#pragma once

#include "realmmesh/cluster/service_discovery_config.hpp"
#include "realmmesh/concurrency/bounded_queue.hpp"
#include "realmmesh/game/gateway/edge_session_table.hpp"
#include "realmmesh/game/gateway/gateway_login_config.hpp"
#include "realmmesh/network/transport/transport_config.hpp"
#include "realmmesh/observability/logger.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
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
    /// 未知或阶段不符的会话命令(如向 pending 会话发送)。
    std::uint64_t unknown_session_commands{0};
    std::uint64_t successful_deliveries{0};
    std::uint64_t failed_deliveries{0};
};

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
    GatewayLoginConfig login;
};

enum class GatewayEventKind : std::uint8_t {
    SessionOpened,
    MessageReceived,
    SessionEstablished,
    SessionClosed,
    PeerAddressChanged,
};

/// 面向业务层的事件:session_id 覆盖 pending 与 established 两个阶段,
/// established 标记区分阶段。已终结会话的迟到帧不会产生事件。
struct GatewayEvent {
    GatewayEventKind kind;
    EdgeSessionId session_id{invalid_edge_session_id};
    network::TransportProtocol protocol{network::TransportProtocol::TlsTcp};
    bool established{false};
    std::vector<std::byte> payload;
};

/// Edge Session 生命周期的唯一所有者:传输层与会话表都收敛到这里的
/// IO 线程,业务层只面对不透明的 EdgeSessionId 与四类会话命令。
/// 契约:try_send 仅对 established 会话生效;终结(accept 失败、decline、
/// close)合成恰好一次 SessionClosed 事件,与传输层是否上报无关。
class GatewayRuntime final {
public:
    explicit GatewayRuntime(
        GatewayConfig config, observability::Logger* logger = nullptr);
    explicit GatewayRuntime(
        GatewayConfig config,
        GatewayRuntimeOptions options,
        observability::Logger* logger = nullptr);
    GatewayRuntime(
        std::vector<std::unique_ptr<network::IMessageTransport>> transports,
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
    [[nodiscard]] const std::vector<network::TransportEndpoint>&
    local_endpoints() const noexcept;

    [[nodiscard]] std::optional<GatewayEvent> try_receive();
    [[nodiscard]] std::vector<GatewayEvent> drain_events(
        std::size_t max_events);

    /// 向 established 会话发送;未知或 pending 会话计入
    /// unknown_session_commands。
    [[nodiscard]] QueueResult try_send(
        EdgeSessionId session_id, std::span<const std::byte> payload);
    /// pending 会话原子地"发送 establish 响应 + 迁入 established";
    /// 成功后发布 SessionEstablished;响应发送失败则终结会话并发布
    /// SessionClosed。
    [[nodiscard]] QueueResult try_accept(
        EdgeSessionId session_id, std::span<const std::byte> response);
    /// 任意阶段会话尽力发送拒绝响应并终结;发布 SessionClosed。
    [[nodiscard]] QueueResult try_decline(
        EdgeSessionId session_id, std::span<const std::byte> response);
    /// 终结任意阶段的会话;发布 SessionClosed。
    [[nodiscard]] QueueResult try_close(EdgeSessionId session_id);
    [[nodiscard]] QueueResult try_reload_credentials();

    [[nodiscard]] GatewayRuntimeStats stats() const noexcept;
    [[nodiscard]] std::optional<std::string> terminal_error() const;

private:
    enum class CommandKind : std::uint8_t {
        Send,
        Accept,
        Decline,
        Close,
        ReloadCredentials,
    };

    struct OutboundCommand {
        CommandKind kind;
        EdgeSessionId session_id{invalid_edge_session_id};
        std::vector<std::byte> payload;
    };

    [[nodiscard]] QueueResult enqueue(OutboundCommand command);
    void io_loop(std::stop_token stop_token) noexcept;
    void process_outbound_commands();
    void process_command(OutboundCommand command);
    /// 本地终结:关闭会话并合成 SessionClosed 事件;未知句柄返回 false。
    [[nodiscard]] bool finish_close(EdgeSessionId session_id);
    /// 轮询传输层并把传输事件翻译为 Edge Session 事件。
    [[nodiscard]] std::vector<GatewayEvent> poll_events(
        std::chrono::milliseconds timeout);
    void publish_event(GatewayEvent event);

    std::vector<std::unique_ptr<network::IMessageTransport>> transports_;
    EdgeSessionTable sessions_;
    GatewayRuntimeOptions options_;
    std::uint16_t local_port_{0};
    std::vector<network::TransportEndpoint> local_endpoints_;
    concurrency::BoundedQueue<GatewayEvent> inbound_;
    concurrency::BoundedQueue<OutboundCommand> outbound_;
    std::jthread io_thread_;
    std::atomic_bool running_{false};
    std::atomic_uint64_t overload_disconnects_{0};
    std::atomic_uint64_t rejected_outbound_commands_{0};
    std::atomic_uint64_t unknown_session_commands_{0};
    std::atomic_uint64_t successful_deliveries_{0};
    std::atomic_uint64_t failed_deliveries_{0};
    mutable std::mutex terminal_error_mutex_;
    std::optional<std::string> terminal_error_;
};

}  // namespace realm::game::gateway
