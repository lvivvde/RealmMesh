#pragma once

// Edge Connection 事件：EdgeRuntime 发布给业务层的轻量值类型。只消费事件的
// 代码包含本头，不必带入 runtime 的私有布局。
#include "realmmesh/edge/edge_connection_table.hpp"
#include "realmmesh/network/transport/message_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace realm::edge {

enum class EdgeEventKind : std::uint8_t {
    ConnectionOpened,
    MessageReceived,
    ConnectionEstablished,
    ConnectionClosed,
    PeerAddressChanged,
};

/// 面向业务层的事件:connection_id 覆盖 pending 与 established 两个阶段,
/// established 标记区分阶段。已终结连接的迟到帧不会产生事件。
struct EdgeEvent {
    EdgeEventKind kind;
    EdgeConnectionId connection_id{invalid_edge_connection_id};
    network::TransportProtocol protocol{network::TransportProtocol::TlsTcp};
    bool established{false};
    std::vector<std::byte> payload;
    std::string source;
};

}  // namespace realm::edge
