#pragma once

// 主传输边界只需事件与 runtime 指针(#128):EdgeRuntime 仅前置声明，
// 完整定义留在生产适配器的实现文件。
#include "realmmesh/edge/edge_connection_table.hpp"
#include "realmmesh/edge/edge_event.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

namespace realm::edge {
class EdgeRuntime;
}  // namespace realm::edge

namespace realm::game::gateway {

enum class PrimaryTransportResult : std::uint8_t {
    Queued,
    Full,
    Stopped,
};

enum class PrimaryTransportCommandKind : std::uint8_t {
    Accept,
    Decline,
    SendHandoff,
    Close,
};

struct PrimaryTransportCommand {
    PrimaryTransportCommandKind kind{PrimaryTransportCommandKind::Close};
    edge::EdgeConnectionId session_id{edge::invalid_edge_connection_id};
    std::vector<std::byte> payload;
};

/// Gateway Login Pipeline 的唯一传输边界。Queued 是所有权提交点；Full
/// 不转移所有权；Stopped 对此适配器实例是终态。
class GatewayPrimaryTransport {
public:
    virtual ~GatewayPrimaryTransport() = default;

    [[nodiscard]] virtual std::vector<edge::EdgeEvent> drain_events(
        std::size_t max_events) = 0;
    [[nodiscard]] virtual PrimaryTransportResult accept(
        edge::EdgeConnectionId session_id, std::span<const std::byte> response) = 0;
    [[nodiscard]] virtual PrimaryTransportResult decline(
        edge::EdgeConnectionId session_id, std::span<const std::byte> response) = 0;
    [[nodiscard]] virtual PrimaryTransportResult send_handoff(
        edge::EdgeConnectionId session_id, std::span<const std::byte> message) = 0;
    [[nodiscard]] virtual PrimaryTransportResult close(
        edge::EdgeConnectionId session_id) = 0;
};

/// 生产适配器只翻译 EdgeRuntime 的事件与命令结果，不持有管线状态。
class EdgeRuntimePrimaryTransport final : public GatewayPrimaryTransport {
public:
    explicit EdgeRuntimePrimaryTransport(edge::EdgeRuntime& runtime)
        : runtime_(&runtime) {}

    [[nodiscard]] std::vector<edge::EdgeEvent> drain_events(
        std::size_t max_events) override;
    [[nodiscard]] PrimaryTransportResult accept(
        edge::EdgeConnectionId session_id,
        std::span<const std::byte> response) override;
    [[nodiscard]] PrimaryTransportResult decline(
        edge::EdgeConnectionId session_id,
        std::span<const std::byte> response) override;
    [[nodiscard]] PrimaryTransportResult send_handoff(
        edge::EdgeConnectionId session_id,
        std::span<const std::byte> message) override;
    [[nodiscard]] PrimaryTransportResult close(
        edge::EdgeConnectionId session_id) override;

private:
    edge::EdgeRuntime* runtime_;
};

/// 可脚本化内存适配器：测试只观察与生产适配器相同的公共契约。
class InMemoryGatewayPrimaryTransport final
    : public GatewayPrimaryTransport {
public:
    void push_event(edge::EdgeEvent event);
    void script_result(
        PrimaryTransportCommandKind kind,
        std::deque<PrimaryTransportResult> results);

    [[nodiscard]] const std::vector<PrimaryTransportCommand>& owned_commands()
        const noexcept {
        return owned_commands_;
    }

    [[nodiscard]] std::vector<edge::EdgeEvent> drain_events(
        std::size_t max_events) override;
    [[nodiscard]] PrimaryTransportResult accept(
        edge::EdgeConnectionId session_id,
        std::span<const std::byte> response) override;
    [[nodiscard]] PrimaryTransportResult decline(
        edge::EdgeConnectionId session_id,
        std::span<const std::byte> response) override;
    [[nodiscard]] PrimaryTransportResult send_handoff(
        edge::EdgeConnectionId session_id,
        std::span<const std::byte> message) override;
    [[nodiscard]] PrimaryTransportResult close(
        edge::EdgeConnectionId session_id) override;

private:
    [[nodiscard]] PrimaryTransportResult execute(
        PrimaryTransportCommandKind kind,
        edge::EdgeConnectionId session_id,
        std::span<const std::byte> payload);

    std::deque<edge::EdgeEvent> events_;
    std::array<std::deque<PrimaryTransportResult>, 4> results_;
    std::vector<PrimaryTransportCommand> owned_commands_;
    bool stopped_{false};
};

}  // namespace realm::game::gateway
