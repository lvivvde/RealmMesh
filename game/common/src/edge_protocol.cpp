#include "realmmesh/game/common/edge_protocol.hpp"

#include "realmmesh/common/v1/envelope.pb.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace realm::game::common {
namespace {

namespace common_v1 = ::realmmesh::protocol::common::v1;
namespace edge_v1 = ::realmmesh::protocol::edge::v1;

/// 本协议当前接受的 message_id。已退役编号 1001/1002/1101/1102/1103/
/// 1104/1201/1202 在此没有分支,parse_envelope 一律按未知消息拒绝。
bool is_known_message_id(std::uint32_t value) {
    switch (static_cast<EdgeMessageId>(value)) {
    case edge_v1::MESSAGE_ID_C2S_HEARTBEAT_REQUEST:
    case edge_v1::MESSAGE_ID_S2C_HEARTBEAT_RESPONSE:
    case edge_v1::MESSAGE_ID_C2S_EDGE_ATTACH:
    case edge_v1::MESSAGE_ID_S2C_EDGE_ATTACH_ACCEPTED:
    case edge_v1::MESSAGE_ID_S2C_ENTER_REALM_GRANTED:
    case edge_v1::MESSAGE_ID_C2S_ENTER_REALM:
    case edge_v1::MESSAGE_ID_S2C_ENTER_REALM_ACCEPTED:
    case edge_v1::MESSAGE_ID_S2C_ERROR:
        return true;
    case edge_v1::MESSAGE_ID_UNSPECIFIED:
        return false;
    }
    return false;
}

std::optional<common_v1::Envelope> parse_envelope(
    std::span<const std::byte> payload) {
    if (payload.size() >
        static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        return std::nullopt;
    }

    common_v1::Envelope envelope;
    if (!envelope.ParseFromArray(
            payload.data(), static_cast<int>(payload.size())) ||
        envelope.protocol_version() != kEdgeProtocolVersion ||
        !is_known_message_id(envelope.message_id())) {
        return std::nullopt;
    }
    return envelope;
}

template <typename Message>
std::vector<std::byte> encode_message(
    const Message& message,
    EdgeMessageId message_id,
    std::uint64_t request_id) {
    std::string serialized_message;
    if (!message.SerializeToString(&serialized_message)) {
        throw std::runtime_error("failed to serialize edge protobuf message");
    }

    common_v1::Envelope envelope;
    envelope.set_protocol_version(kEdgeProtocolVersion);
    envelope.set_message_id(static_cast<std::uint32_t>(message_id));
    envelope.set_request_id(request_id);
    envelope.set_payload(std::move(serialized_message));

    std::string serialized_envelope;
    if (!envelope.SerializeToString(&serialized_envelope)) {
        throw std::runtime_error("failed to serialize edge protobuf envelope");
    }
    const auto* begin =
        reinterpret_cast<const std::byte*>(serialized_envelope.data());
    return {begin, begin + serialized_envelope.size()};
}

template <typename Message>
std::optional<Message> decode_message(
    std::span<const std::byte> payload, EdgeMessageId expected_message_id) {
    const auto envelope = parse_envelope(payload);
    if (!envelope.has_value() ||
        envelope->message_id() !=
            static_cast<std::uint32_t>(expected_message_id) ||
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

}  // namespace

std::optional<EdgeMessageId> edge_message_id(
    std::span<const std::byte> payload) {
    const auto envelope = parse_envelope(payload);
    if (!envelope.has_value()) return std::nullopt;
    return static_cast<EdgeMessageId>(envelope->message_id());
}

std::optional<std::uint64_t> edge_request_id(
    std::span<const std::byte> payload) {
    const auto envelope = parse_envelope(payload);
    if (!envelope.has_value()) return std::nullopt;
    return envelope->request_id();
}

std::vector<std::byte> encode(
    const HeartbeatRequest& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_C2S_HEARTBEAT_REQUEST, request_id);
}

std::vector<std::byte> encode(
    const HeartbeatResponse& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_S2C_HEARTBEAT_RESPONSE, request_id);
}

std::vector<std::byte> encode(
    const EdgeAttach& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_C2S_EDGE_ATTACH, request_id);
}

std::vector<std::byte> encode(
    const EdgeAttachAccepted& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_S2C_EDGE_ATTACH_ACCEPTED, request_id);
}

std::vector<std::byte> encode(
    const EnterRealmGranted& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_S2C_ENTER_REALM_GRANTED, request_id);
}

std::vector<std::byte> encode(
    const EnterRealm& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_C2S_ENTER_REALM, request_id);
}

std::vector<std::byte> encode(
    const EnterRealmAccepted& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_S2C_ENTER_REALM_ACCEPTED, request_id);
}

std::vector<std::byte> encode(
    const EdgeError& message, std::uint64_t request_id) {
    return encode_message(message, edge_v1::MESSAGE_ID_S2C_ERROR, request_id);
}

std::optional<HeartbeatRequest> decode_heartbeat_request(
    std::span<const std::byte> payload) {
    return decode_message<HeartbeatRequest>(
        payload, edge_v1::MESSAGE_ID_C2S_HEARTBEAT_REQUEST);
}

std::optional<HeartbeatResponse> decode_heartbeat_response(
    std::span<const std::byte> payload) {
    return decode_message<HeartbeatResponse>(
        payload, edge_v1::MESSAGE_ID_S2C_HEARTBEAT_RESPONSE);
}

std::optional<EdgeAttach> decode_edge_attach(
    std::span<const std::byte> payload) {
    return decode_message<EdgeAttach>(
        payload, edge_v1::MESSAGE_ID_C2S_EDGE_ATTACH);
}

std::optional<EdgeAttachAccepted> decode_edge_attach_accepted(
    std::span<const std::byte> payload) {
    return decode_message<EdgeAttachAccepted>(
        payload, edge_v1::MESSAGE_ID_S2C_EDGE_ATTACH_ACCEPTED);
}

std::optional<EnterRealmGranted> decode_enter_realm_granted(
    std::span<const std::byte> payload) {
    return decode_message<EnterRealmGranted>(
        payload, edge_v1::MESSAGE_ID_S2C_ENTER_REALM_GRANTED);
}

std::optional<EnterRealm> decode_enter_realm(
    std::span<const std::byte> payload) {
    return decode_message<EnterRealm>(
        payload, edge_v1::MESSAGE_ID_C2S_ENTER_REALM);
}

std::optional<EnterRealmAccepted> decode_enter_realm_accepted(
    std::span<const std::byte> payload) {
    return decode_message<EnterRealmAccepted>(
        payload, edge_v1::MESSAGE_ID_S2C_ENTER_REALM_ACCEPTED);
}

std::optional<EdgeError> decode_edge_error(std::span<const std::byte> payload) {
    return decode_message<EdgeError>(payload, edge_v1::MESSAGE_ID_S2C_ERROR);
}

}  // namespace realm::game::common
