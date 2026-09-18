#pragma once

#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <vector>

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
    EdgeSessionId session_id{invalid_edge_session_id};
    std::vector<std::byte> payload;
};

/// Gateway Login Pipeline 的唯一传输边界。Queued 是所有权提交点；Full
/// 不转移所有权；Stopped 对此适配器实例是终态。
class GatewayPrimaryTransport {
public:
    virtual ~GatewayPrimaryTransport() = default;

    [[nodiscard]] virtual std::vector<GatewayEvent> drain_events(
        std::size_t max_events) = 0;
    [[nodiscard]] virtual PrimaryTransportResult accept(
        EdgeSessionId session_id, std::span<const std::byte> response) = 0;
    [[nodiscard]] virtual PrimaryTransportResult decline(
        EdgeSessionId session_id, std::span<const std::byte> response) = 0;
    [[nodiscard]] virtual PrimaryTransportResult send_handoff(
        EdgeSessionId session_id, std::span<const std::byte> message) = 0;
    [[nodiscard]] virtual PrimaryTransportResult close(
        EdgeSessionId session_id) = 0;
};

/// 生产适配器只翻译 GatewayRuntime 的事件与命令结果，不持有管线状态。
class GatewayRuntimePrimaryTransport final : public GatewayPrimaryTransport {
public:
    explicit GatewayRuntimePrimaryTransport(GatewayRuntime& runtime)
        : runtime_(&runtime) {}

    [[nodiscard]] std::vector<GatewayEvent> drain_events(
        std::size_t max_events) override;
    [[nodiscard]] PrimaryTransportResult accept(
        EdgeSessionId session_id,
        std::span<const std::byte> response) override;
    [[nodiscard]] PrimaryTransportResult decline(
        EdgeSessionId session_id,
        std::span<const std::byte> response) override;
    [[nodiscard]] PrimaryTransportResult send_handoff(
        EdgeSessionId session_id,
        std::span<const std::byte> message) override;
    [[nodiscard]] PrimaryTransportResult close(
        EdgeSessionId session_id) override;

private:
    GatewayRuntime* runtime_;
};

/// 可脚本化内存适配器：测试只观察与生产适配器相同的公共契约。
class InMemoryGatewayPrimaryTransport final
    : public GatewayPrimaryTransport {
public:
    void push_event(GatewayEvent event);
    void script_result(
        PrimaryTransportCommandKind kind,
        std::deque<PrimaryTransportResult> results);

    [[nodiscard]] const std::vector<PrimaryTransportCommand>& owned_commands()
        const noexcept {
        return owned_commands_;
    }

    [[nodiscard]] std::vector<GatewayEvent> drain_events(
        std::size_t max_events) override;
    [[nodiscard]] PrimaryTransportResult accept(
        EdgeSessionId session_id,
        std::span<const std::byte> response) override;
    [[nodiscard]] PrimaryTransportResult decline(
        EdgeSessionId session_id,
        std::span<const std::byte> response) override;
    [[nodiscard]] PrimaryTransportResult send_handoff(
        EdgeSessionId session_id,
        std::span<const std::byte> message) override;
    [[nodiscard]] PrimaryTransportResult close(
        EdgeSessionId session_id) override;

private:
    [[nodiscard]] PrimaryTransportResult execute(
        PrimaryTransportCommandKind kind,
        EdgeSessionId session_id,
        std::span<const std::byte> payload);

    std::deque<GatewayEvent> events_;
    std::array<std::deque<PrimaryTransportResult>, 4> results_;
    std::vector<PrimaryTransportCommand> owned_commands_;
    bool stopped_{false};
};

}  // namespace realm::game::gateway
