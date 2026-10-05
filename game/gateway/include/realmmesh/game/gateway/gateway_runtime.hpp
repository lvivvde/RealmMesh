#pragma once

#include "realmmesh/concurrency/bounded_queue.hpp"
#include "realmmesh/game/gateway/edge_session_table.hpp"
#include "realmmesh/game/gateway/gateway_config.hpp"
#include "realmmesh/game/gateway/gateway_event.hpp"
#include "realmmesh/game/gateway/gateway_ingress.hpp"
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
    std::uint64_t invalid_source_disconnects{0};
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
    GatewayRuntime(
        std::vector<std::unique_ptr<network::IMessageTransport>> transports,
        GatewayRuntimeOptions options,
        GatewaySourceConfig source_config);
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
    GatewaySourceNormalizer source_normalizer_;
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
    std::atomic_uint64_t invalid_source_disconnects_{0};
    mutable std::mutex terminal_error_mutex_;
    std::optional<std::string> terminal_error_;
};

}  // namespace realm::game::gateway
