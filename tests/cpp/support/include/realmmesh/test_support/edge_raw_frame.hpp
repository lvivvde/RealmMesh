#pragma once

#include "realmmesh/common/v1/envelope.pb.h"
#include "realmmesh/game/common/edge_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace realm::test_support {

/// 用任意(含已退役)编号手工组帧。
///
/// 编码面不提供已退役消息的构造入口,只能绕过 `game::common::encode` 直接写
/// `Envelope`,才能验证解码面真的拒绝它们。
[[nodiscard]] inline std::vector<std::byte> edge_raw_frame(
    std::uint32_t message_id, std::uint64_t request_id) {
    ::realmmesh::protocol::common::v1::Envelope envelope;
    envelope.set_protocol_version(game::common::kEdgeProtocolVersion);
    envelope.set_message_id(message_id);
    envelope.set_request_id(request_id);
    envelope.set_payload("retired");
    std::string serialized;
    if (!envelope.SerializeToString(&serialized)) {
        throw std::runtime_error("cannot serialize envelope");
    }
    const auto* begin = reinterpret_cast<const std::byte*>(serialized.data());
    return {begin, begin + serialized.size()};
}

}  // namespace realm::test_support
