#pragma once

// Edge 与 Realm 两组消息共用的 common.v1.Envelope 编解码(仅本库内部)。
// message_id 白名单由各组自己的 *_message_id 判定,本层只查版本与编号相等。

#include "realmmesh/common/v1/envelope.pb.h"
#include "realmmesh/game/common/edge_protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace realm::game::common::detail {

[[nodiscard]] inline std::optional<::realmmesh::protocol::common::v1::Envelope>
parse_envelope(std::span<const std::byte> payload) {
    if (payload.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    ::realmmesh::protocol::common::v1::Envelope envelope;
    if (!envelope.ParseFromArray(
            payload.data(), static_cast<int>(payload.size())) ||
        envelope.protocol_version() != kEdgeProtocolVersion) {
        return std::nullopt;
    }
    return envelope;
}

template <typename Message>
[[nodiscard]] std::vector<std::byte> encode_message(
    const Message& message,
    std::uint32_t message_id,
    std::uint64_t request_id) {
    std::string serialized_message;
    if (!message.SerializeToString(&serialized_message)) {
        throw std::runtime_error("failed to serialize protobuf message");
    }

    ::realmmesh::protocol::common::v1::Envelope envelope;
    envelope.set_protocol_version(kEdgeProtocolVersion);
    envelope.set_message_id(message_id);
    envelope.set_request_id(request_id);
    envelope.set_payload(std::move(serialized_message));

    std::string serialized_envelope;
    if (!envelope.SerializeToString(&serialized_envelope)) {
        throw std::runtime_error("failed to serialize protobuf envelope");
    }
    const auto* begin =
        reinterpret_cast<const std::byte*>(serialized_envelope.data());
    return {begin, begin + serialized_envelope.size()};
}

template <typename Message>
[[nodiscard]] std::optional<Message> decode_message(
    std::span<const std::byte> payload, std::uint32_t expected_message_id) {
    const auto envelope = parse_envelope(payload);
    if (!envelope.has_value() ||
        envelope->message_id() != expected_message_id ||
        envelope->payload().size() >
            static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    Message message;
    if (!message.ParseFromArray(
            envelope->payload().data(),
            static_cast<int>(envelope->payload().size()))) {
        return std::nullopt;
    }
    return message;
}

}  // namespace realm::game::common::detail
