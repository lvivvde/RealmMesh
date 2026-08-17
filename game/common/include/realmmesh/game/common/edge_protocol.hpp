#pragma once

#include "realmmesh/edge/v1/edge.pb.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace realm::game::common {

using ServiceEndpoint = ::realmmesh::protocol::edge::v1::ServiceEndpoint;
using LoginRequest = ::realmmesh::protocol::edge::v1::LoginRequest;
using LoginSucceeded = ::realmmesh::protocol::edge::v1::LoginSucceeded;
using RealmAuthenticate = ::realmmesh::protocol::edge::v1::RealmAuthenticate;
using CharacterSummary = ::realmmesh::protocol::edge::v1::CharacterSummary;
using CharacterList = ::realmmesh::protocol::edge::v1::CharacterList;
using SelectCharacter = ::realmmesh::protocol::edge::v1::SelectCharacter;
using EnterGameIssued = ::realmmesh::protocol::edge::v1::EnterGameIssued;
using EnterGame = ::realmmesh::protocol::edge::v1::EnterGame;
using EnterGameAccepted = ::realmmesh::protocol::edge::v1::EnterGameAccepted;
using EdgeError = ::realmmesh::protocol::edge::v1::EdgeError;
using EdgeMessageId = ::realmmesh::protocol::edge::v1::MessageId;

inline constexpr std::uint32_t kEdgeProtocolVersion = 1;

[[nodiscard]] inline std::span<const std::byte> protobuf_bytes(
    std::string_view value) noexcept {
    return {
        reinterpret_cast<const std::byte*>(value.data()),
        value.size(),
    };
}

[[nodiscard]] std::optional<EdgeMessageId> edge_message_id(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<std::uint64_t> edge_request_id(
    std::span<const std::byte> payload);

[[nodiscard]] std::vector<std::byte> encode(
    const LoginRequest& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const LoginSucceeded& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const RealmAuthenticate& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const CharacterList& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const SelectCharacter& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterGameIssued& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterGame& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterGameAccepted& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EdgeError& message, std::uint64_t request_id = 0);

[[nodiscard]] std::optional<LoginRequest> decode_login_request(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<LoginSucceeded> decode_login_succeeded(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<RealmAuthenticate> decode_realm_authenticate(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<CharacterList> decode_character_list(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<SelectCharacter> decode_select_character(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterGameIssued> decode_enter_game_issued(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterGame> decode_enter_game(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterGameAccepted> decode_enter_game_accepted(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EdgeError> decode_edge_error(
    std::span<const std::byte> payload);

}  // namespace realm::game::common
