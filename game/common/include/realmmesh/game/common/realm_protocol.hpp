#pragma once

#include "realmmesh/realm/v1/realm.pb.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace realm::game::common {

// Realm Session 业务消息(#93)。与 Edge 消息共用 Envelope 与 1999 EdgeError;
// 错误码常量见 edge_protocol.hpp(3003-3010)。
using CharacterSummary = ::realmmesh::protocol::realm::v1::CharacterSummary;
using ListCharacters = ::realmmesh::protocol::realm::v1::ListCharacters;
using CharacterList = ::realmmesh::protocol::realm::v1::CharacterList;
using CreateCharacter = ::realmmesh::protocol::realm::v1::CreateCharacter;
using CharacterCreated = ::realmmesh::protocol::realm::v1::CharacterCreated;
using SelectCharacter = ::realmmesh::protocol::realm::v1::SelectCharacter;
using CharacterSelected = ::realmmesh::protocol::realm::v1::CharacterSelected;
using Train = ::realmmesh::protocol::realm::v1::Train;
using TrainResult = ::realmmesh::protocol::realm::v1::TrainResult;
using RealmSessionDisplaced =
    ::realmmesh::protocol::realm::v1::RealmSessionDisplaced;
using RealmMessageId = ::realmmesh::protocol::realm::v1::MessageId;

/// 只认领 1401-1409;Edge 编号(含 1999)交给 edge_message_id。
[[nodiscard]] std::optional<RealmMessageId> realm_message_id(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<std::uint64_t> realm_request_id(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode(
    const ListCharacters& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const CharacterList& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const CreateCharacter& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const CharacterCreated& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const SelectCharacter& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const CharacterSelected& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const Train& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const TrainResult& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const RealmSessionDisplaced& message, std::uint64_t request_id = 0);

[[nodiscard]] std::optional<ListCharacters> decode_list_characters(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<CharacterList> decode_character_list(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<CreateCharacter> decode_create_character(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<CharacterCreated> decode_character_created(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<SelectCharacter> decode_select_character(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<CharacterSelected> decode_character_selected(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<Train> decode_train(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<TrainResult> decode_train_result(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<RealmSessionDisplaced>
decode_realm_session_displaced(std::span<const std::byte> payload);

}  // namespace realm::game::common
