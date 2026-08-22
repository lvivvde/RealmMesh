#include "realmmesh/game/common/edge_protocol.hpp"

#include "realmmesh/common/v1/envelope.pb.h"

#include <limits>
#include <stdexcept>
#include <string>

namespace realm::game::common {
namespace {

namespace common_v1 = ::realmmesh::protocol::common::v1;
namespace edge_v1 = ::realmmesh::protocol::edge::v1;

bool is_known_message_id(std::uint32_t value) {
    switch (static_cast<EdgeMessageId>(value)) {
    case edge_v1::MESSAGE_ID_C2S_LOGIN_REQUEST:
    case edge_v1::MESSAGE_ID_S2C_LOGIN_SUCCEEDED:
    case edge_v1::MESSAGE_ID_C2S_REALM_AUTHENTICATE:
    case edge_v1::MESSAGE_ID_S2C_CHARACTER_LIST:
    case edge_v1::MESSAGE_ID_C2S_SELECT_CHARACTER:
    case edge_v1::MESSAGE_ID_S2C_ENTER_GAME_ISSUED:
    case edge_v1::MESSAGE_ID_C2S_HEARTBEAT_REQUEST:
    case edge_v1::MESSAGE_ID_S2C_HEARTBEAT_RESPONSE:
    case edge_v1::MESSAGE_ID_C2S_ENTER_GAME:
    case edge_v1::MESSAGE_ID_S2C_ENTER_GAME_ACCEPTED:
    case edge_v1::MESSAGE_ID_S2C_ERROR:
        return true;
    case edge_v1::MESSAGE_ID_UNSPECIFIED:
        return false;
    }
    return false;
}

std::optional<common_v1::Envelope> parse_envelope(
    std::span<const std::byte> payload) {
    if (payload.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
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
    const auto* begin = reinterpret_cast<const std::byte*>(
        serialized_envelope.data());
    return {begin, begin + serialized_envelope.size()};
}

template <typename Message>
std::optional<Message> decode_message(
    std::span<const std::byte> payload,
    EdgeMessageId expected_message_id) {
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
    const LoginRequest& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_C2S_LOGIN_REQUEST, request_id);
}

std::vector<std::byte> encode(
    const LoginSucceeded& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_S2C_LOGIN_SUCCEEDED, request_id);
}

std::vector<std::byte> encode(
    const RealmAuthenticate& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_C2S_REALM_AUTHENTICATE, request_id);
}

std::vector<std::byte> encode(
    const CharacterList& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_S2C_CHARACTER_LIST, request_id);
}

std::vector<std::byte> encode(
    const SelectCharacter& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_C2S_SELECT_CHARACTER, request_id);
}

std::vector<std::byte> encode(
    const EnterGameIssued& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_S2C_ENTER_GAME_ISSUED, request_id);
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
    const EnterGame& message, std::uint64_t request_id) {
    return encode_message(message, edge_v1::MESSAGE_ID_C2S_ENTER_GAME, request_id);
}

std::vector<std::byte> encode(
    const EnterGameAccepted& message, std::uint64_t request_id) {
    return encode_message(
        message, edge_v1::MESSAGE_ID_S2C_ENTER_GAME_ACCEPTED, request_id);
}

std::vector<std::byte> encode(
    const EdgeError& message, std::uint64_t request_id) {
    return encode_message(message, edge_v1::MESSAGE_ID_S2C_ERROR, request_id);
}

std::optional<LoginRequest> decode_login_request(
    std::span<const std::byte> payload) {
    return decode_message<LoginRequest>(
        payload, edge_v1::MESSAGE_ID_C2S_LOGIN_REQUEST);
}

std::optional<LoginSucceeded> decode_login_succeeded(
    std::span<const std::byte> payload) {
    return decode_message<LoginSucceeded>(
        payload, edge_v1::MESSAGE_ID_S2C_LOGIN_SUCCEEDED);
}

std::optional<RealmAuthenticate> decode_realm_authenticate(
    std::span<const std::byte> payload) {
    return decode_message<RealmAuthenticate>(
        payload, edge_v1::MESSAGE_ID_C2S_REALM_AUTHENTICATE);
}

std::optional<CharacterList> decode_character_list(
    std::span<const std::byte> payload) {
    return decode_message<CharacterList>(
        payload, edge_v1::MESSAGE_ID_S2C_CHARACTER_LIST);
}

std::optional<SelectCharacter> decode_select_character(
    std::span<const std::byte> payload) {
    return decode_message<SelectCharacter>(
        payload, edge_v1::MESSAGE_ID_C2S_SELECT_CHARACTER);
}

std::optional<EnterGameIssued> decode_enter_game_issued(
    std::span<const std::byte> payload) {
    return decode_message<EnterGameIssued>(
        payload, edge_v1::MESSAGE_ID_S2C_ENTER_GAME_ISSUED);
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

std::optional<EnterGame> decode_enter_game(
    std::span<const std::byte> payload) {
    return decode_message<EnterGame>(
        payload, edge_v1::MESSAGE_ID_C2S_ENTER_GAME);
}

std::optional<EnterGameAccepted> decode_enter_game_accepted(
    std::span<const std::byte> payload) {
    return decode_message<EnterGameAccepted>(
        payload, edge_v1::MESSAGE_ID_S2C_ENTER_GAME_ACCEPTED);
}

std::optional<EdgeError> decode_edge_error(
    std::span<const std::byte> payload) {
    return decode_message<EdgeError>(payload, edge_v1::MESSAGE_ID_S2C_ERROR);
}

}  // namespace realm::game::common
