#include "realmmesh/game/common/realm_protocol.hpp"

#include "envelope_codec.hpp"

namespace realm::game::common {
namespace {

namespace realm_v1 = ::realmmesh::protocol::realm::v1;

bool is_realm_message_id(std::uint32_t value) {
    switch (static_cast<RealmMessageId>(value)) {
    case realm_v1::MESSAGE_ID_C2S_LIST_CHARACTERS:
    case realm_v1::MESSAGE_ID_S2C_CHARACTER_LIST:
    case realm_v1::MESSAGE_ID_C2S_CREATE_CHARACTER:
    case realm_v1::MESSAGE_ID_S2C_CHARACTER_CREATED:
    case realm_v1::MESSAGE_ID_C2S_SELECT_CHARACTER:
    case realm_v1::MESSAGE_ID_S2C_CHARACTER_SELECTED:
    case realm_v1::MESSAGE_ID_C2S_TRAIN:
    case realm_v1::MESSAGE_ID_S2C_TRAIN_RESULT:
    case realm_v1::MESSAGE_ID_S2C_REALM_SESSION_DISPLACED:
        return true;
    case realm_v1::MESSAGE_ID_UNSPECIFIED:
        return false;
    default:
        return false;
    }
}

std::optional<::realmmesh::protocol::common::v1::Envelope>
parse_realm_envelope(std::span<const std::byte> payload) {
    auto envelope = detail::parse_envelope(payload);
    if (!envelope.has_value() || !is_realm_message_id(envelope->message_id())) {
        return std::nullopt;
    }
    return envelope;
}

}  // namespace

std::optional<RealmMessageId> realm_message_id(
    std::span<const std::byte> payload) {
    const auto envelope = parse_realm_envelope(payload);
    if (!envelope.has_value()) return std::nullopt;
    return static_cast<RealmMessageId>(envelope->message_id());
}

std::optional<std::uint64_t> realm_request_id(
    std::span<const std::byte> payload) {
    const auto envelope = parse_realm_envelope(payload);
    if (!envelope.has_value()) return std::nullopt;
    return envelope->request_id();
}

std::vector<std::byte> encode(
    const ListCharacters& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_C2S_LIST_CHARACTERS, request_id);
}

std::vector<std::byte> encode(
    const CharacterList& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_S2C_CHARACTER_LIST, request_id);
}

std::vector<std::byte> encode(
    const CreateCharacter& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_C2S_CREATE_CHARACTER, request_id);
}

std::vector<std::byte> encode(
    const CharacterCreated& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_S2C_CHARACTER_CREATED, request_id);
}

std::vector<std::byte> encode(
    const SelectCharacter& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_C2S_SELECT_CHARACTER, request_id);
}

std::vector<std::byte> encode(
    const CharacterSelected& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_S2C_CHARACTER_SELECTED, request_id);
}

std::vector<std::byte> encode(
    const Train& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_C2S_TRAIN, request_id);
}

std::vector<std::byte> encode(
    const TrainResult& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_S2C_TRAIN_RESULT, request_id);
}

std::vector<std::byte> encode(
    const RealmSessionDisplaced& message, std::uint64_t request_id) {
    return detail::encode_message(
        message, realm_v1::MESSAGE_ID_S2C_REALM_SESSION_DISPLACED, request_id);
}

std::optional<ListCharacters> decode_list_characters(
    std::span<const std::byte> payload) {
    return detail::decode_message<ListCharacters>(
        payload, realm_v1::MESSAGE_ID_C2S_LIST_CHARACTERS);
}

std::optional<CharacterList> decode_character_list(
    std::span<const std::byte> payload) {
    return detail::decode_message<CharacterList>(
        payload, realm_v1::MESSAGE_ID_S2C_CHARACTER_LIST);
}

std::optional<CreateCharacter> decode_create_character(
    std::span<const std::byte> payload) {
    return detail::decode_message<CreateCharacter>(
        payload, realm_v1::MESSAGE_ID_C2S_CREATE_CHARACTER);
}

std::optional<CharacterCreated> decode_character_created(
    std::span<const std::byte> payload) {
    return detail::decode_message<CharacterCreated>(
        payload, realm_v1::MESSAGE_ID_S2C_CHARACTER_CREATED);
}

std::optional<SelectCharacter> decode_select_character(
    std::span<const std::byte> payload) {
    return detail::decode_message<SelectCharacter>(
        payload, realm_v1::MESSAGE_ID_C2S_SELECT_CHARACTER);
}

std::optional<CharacterSelected> decode_character_selected(
    std::span<const std::byte> payload) {
    return detail::decode_message<CharacterSelected>(
        payload, realm_v1::MESSAGE_ID_S2C_CHARACTER_SELECTED);
}

std::optional<Train> decode_train(
    std::span<const std::byte> payload) {
    return detail::decode_message<Train>(
        payload, realm_v1::MESSAGE_ID_C2S_TRAIN);
}

std::optional<TrainResult> decode_train_result(
    std::span<const std::byte> payload) {
    return detail::decode_message<TrainResult>(
        payload, realm_v1::MESSAGE_ID_S2C_TRAIN_RESULT);
}

std::optional<RealmSessionDisplaced> decode_realm_session_displaced(
    std::span<const std::byte> payload) {
    return detail::decode_message<RealmSessionDisplaced>(
        payload, realm_v1::MESSAGE_ID_S2C_REALM_SESSION_DISPLACED);
}

}  // namespace realm::game::common
