#pragma once

// Gateway 事件(#128):GatewayRuntime 与 GatewayPrimaryTransport 共用的
// 轻量值类型。只消费事件的代码包含本头，不必带入 runtime 的私有布局。
#include "realmmesh/game/gateway/edge_session_table.hpp"
#include "realmmesh/network/transport/message_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace realm::game::gateway {

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
    std::string source;
};

}  // namespace realm::game::gateway
