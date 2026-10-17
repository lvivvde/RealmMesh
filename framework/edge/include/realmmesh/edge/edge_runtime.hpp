#pragma once

#include "realmmesh/concurrency/bounded_queue.hpp"
#include "realmmesh/edge/edge_connection_table.hpp"
#include "realmmesh/edge/edge_event.hpp"
#include "realmmesh/edge/edge_runtime_options.hpp"
#include "realmmesh/edge/edge_source_address.hpp"
#include "realmmesh/network/transport/message_transport.hpp"
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

namespace realm::edge {

enum class QueueResult : std::uint8_t {
    Queued,
    Full,
    Stopped,
};

struct EdgeRuntimeStats {
    std::uint64_t overload_disconnects{0};
    std::uint64_t rejected_outbound_commands{0};
    /// 未知或阶段不符的连接命令(如向 pending 连接发送)。
    std::uint64_t unknown_session_commands{0};
    std::uint64_t successful_deliveries{0};
    std::uint64_t failed_deliveries{0};
    std::uint64_t invalid_source_disconnects{0};
};

/// Edge Connection 生命周期的唯一所有者:传输层与连接表都收敛到这里的
/// IO 线程,业务层只面对不透明的 EdgeConnectionId 与四类连接命令。
/// 契约:try_send 仅对 established 连接生效;终结(accept 失败、decline、
/// close)合成恰好一次 ConnectionClosed 事件,与传输层是否上报无关。
class EdgeRuntime final {
public:
    /// 按配置创建全部启用的传输(QUIC、TLS/TCP)。
    EdgeRuntime(
        const std::vector<network::TransportConfig>& transports,
        EdgeRuntimeOptions options,
        EdgeSourceConfig source_config = {},
        observability::Logger* logger = nullptr);
    EdgeRuntime(
        std::vector<std::unique_ptr<network::IMessageTransport>> transports,
        EdgeRuntimeOptions options);
    EdgeRuntime(
        std::vector<std::unique_ptr<network::IMessageTransport>> transports,
        EdgeRuntimeOptions options,
        EdgeSourceConfig source_config);
    ~EdgeRuntime();

    EdgeRuntime(const EdgeRuntime&) = delete;
    EdgeRuntime& operator=(const EdgeRuntime&) = delete;

    void start();
    void stop() noexcept;
    [[nodiscard]] bool running() const noexcept;

    [[nodiscard]] std::uint16_t local_port() const noexcept;
    [[nodiscard]] std::optional<network::TransportEndpoint> local_endpoint(
        std::string_view transport_name) const;
    [[nodiscard]] const std::vector<network::TransportEndpoint>&
    local_endpoints() const noexcept;

    [[nodiscard]] std::optional<EdgeEvent> try_receive();
    [[nodiscard]] std::vector<EdgeEvent> drain_events(
        std::size_t max_events);

    /// 向 established 连接发送;未知或 pending 连接计入
    /// unknown_session_commands。
    [[nodiscard]] QueueResult try_send(
        EdgeConnectionId connection_id, std::span<const std::byte> payload);
    /// pending 连接原子地"发送 establish 响应 + 迁入 established";
    /// 成功后发布 ConnectionEstablished;响应发送失败则终结连接并发布
    /// ConnectionClosed。
    [[nodiscard]] QueueResult try_accept(
        EdgeConnectionId connection_id, std::span<const std::byte> response);
    /// 任意阶段连接尽力发送拒绝响应并终结;发布 ConnectionClosed。
    [[nodiscard]] QueueResult try_decline(
        EdgeConnectionId connection_id, std::span<const std::byte> response);
    /// 终结任意阶段的连接;发布 ConnectionClosed。
    [[nodiscard]] QueueResult try_close(EdgeConnectionId connection_id);
    [[nodiscard]] QueueResult try_reload_credentials();

    [[nodiscard]] EdgeRuntimeStats stats() const noexcept;
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
        EdgeConnectionId connection_id{invalid_edge_connection_id};
        std::vector<std::byte> payload;
    };

    [[nodiscard]] QueueResult enqueue(OutboundCommand command);
    void io_loop(std::stop_token stop_token) noexcept;
    void process_outbound_commands();
    void process_command(OutboundCommand command);
    /// 本地终结:关闭连接并合成 ConnectionClosed 事件;未知句柄返回 false。
    [[nodiscard]] bool finish_close(EdgeConnectionId connection_id);
    /// 轮询传输层并把传输事件翻译为 Edge Connection 事件。
    [[nodiscard]] std::vector<EdgeEvent> poll_events(
        std::chrono::milliseconds timeout);
    void publish_event(EdgeEvent event);

    std::vector<std::unique_ptr<network::IMessageTransport>> transports_;
    EdgeConnectionTable connections_;
    EdgeRuntimeOptions options_;
    EdgeSourceNormalizer source_normalizer_;
    std::uint16_t local_port_{0};
    std::vector<network::TransportEndpoint> local_endpoints_;
    concurrency::BoundedQueue<EdgeEvent> inbound_;
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

}  // namespace realm::edge
