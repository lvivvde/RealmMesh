#pragma once

#include "realmmesh/network/transport/message_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realm::game::gateway {

/// Edge Session 的不透明标识:会话表在连接打开时分配,自打开稳定到关闭、
/// 终结后不复用;0 保留为无效值(见 CONTEXT.md:Edge Session)。
struct EdgeSessionId {
    std::uint64_t value{0};

    bool operator==(const EdgeSessionId&) const = default;
};

inline constexpr EdgeSessionId invalid_edge_session_id{0};

}  // namespace realm::game::gateway

template <>
struct std::hash<realm::game::gateway::EdgeSessionId> {
    std::size_t operator()(
        const realm::game::gateway::EdgeSessionId& id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};

namespace realm::game::gateway {

/// Edge Session 当前唯一的传输通道(见 CONTEXT.md:Primary Transport);
/// QUIC 与 TLS/TCP 只是建连时的二选一,不存在会话内第二通道。
struct PrimaryTransport {
    std::string transport_name;
    network::TransportProtocol protocol{network::TransportProtocol::TlsTcp};
    network::SessionId transport_session_id{network::invalid_session_id};

    bool operator==(const PrimaryTransport&) const = default;
};

enum class SendResult : std::uint8_t {
    Sent,
    UnknownSession,
    SendFailed,
};

/// 会话表中一条 Edge Session 的当前快照:主通道 + 生命周期阶段。
struct EdgeSessionRecord {
    PrimaryTransport primary;
    std::string source;
    bool established{false};
};

/// 一次本地终结的即时快照:记录移除与传输层关闭在同一点发生。
struct ClosedEdgeSession {
    EdgeSessionId session_id;
    std::string source;
    bool established{false};
};

/// Edge Session 表:会话状态(pending/established、主通道)的唯一事实来源。
/// 单一 id 空间按 EdgeSessionId 寻址,传输层会话可反查句柄。
/// 单线程约定:所有操作只发生在 GatewayRuntime 的 IO 线程;本表不发事件、
/// 不做业务通知——终结通知由 GatewayRuntime 合成与转发。
class EdgeSessionTable final {
public:
    void register_transport(network::IMessageTransport& transport);

    /// pending 阶段开表;对同一传输层会话重复 open 幂等返回既有句柄;
    /// id 耗尽抛 std::overflow_error。
    [[nodiscard]] EdgeSessionId open(
        std::string_view transport_name,
        network::SessionId transport_session_id,
        std::string source);

    [[nodiscard]] bool update_source(
        EdgeSessionId session_id, std::string source);

    /// pending → established;未知句柄或已建立返回 false。
    [[nodiscard]] bool establish(EdgeSessionId session_id);

    /// 由传输层会话反查句柄;未知(含已终结)返回 nullopt。
    [[nodiscard]] std::optional<EdgeSessionId> find(
        std::string_view transport_name,
        network::SessionId transport_session_id) const;

    [[nodiscard]] std::optional<EdgeSessionRecord> record(
        EdgeSessionId session_id) const;

    /// 传输层报告会话终结(对端关闭/超时/错误路径):移除记录并返回快照;
    /// 未知会话返回 nullopt(如终结已由本地处理,避免二次通知)。
    [[nodiscard]] std::optional<ClosedEdgeSession> on_transport_closed(
        std::string_view transport_name,
        network::SessionId transport_session_id);

    /// 任意阶段发送;established-only 的对外契约由 GatewayRuntime 强制,
    /// pending 阶段的拒绝响应经由本方法送达。
    [[nodiscard]] SendResult send(
        EdgeSessionId session_id, std::span<const std::byte> payload);

    /// 本地关闭:通知传输层、移除记录、返回快照;未知句柄返回 nullopt。
    /// TLS 传输层对本地 close 不产生 SessionClosed 事件(仅对端关闭/超时
    /// 会上报),因此调用方必须依据快照合成终结事件,保证业务层对每条
    /// Edge Session 恰好收到一次终结通知。
    [[nodiscard]] std::optional<ClosedEdgeSession> close_session(
        EdgeSessionId session_id);

private:
    using Table = std::unordered_map<EdgeSessionId, EdgeSessionRecord>;

    [[nodiscard]] network::IMessageTransport* find_transport(
        std::string_view transport_name) const;
    void erase(Table::iterator session);

    std::unordered_map<std::string, network::IMessageTransport*> transports_;
    Table sessions_;
    std::unordered_map<
        std::string,
        std::unordered_map<network::SessionId, EdgeSessionId>>
        by_transport_;
    EdgeSessionId next_session_id_{invalid_edge_session_id.value + 1};
};

}  // namespace realm::game::gateway
