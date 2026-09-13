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
using HeartbeatRequest = ::realmmesh::protocol::edge::v1::HeartbeatRequest;
using HeartbeatResponse = ::realmmesh::protocol::edge::v1::HeartbeatResponse;
using EnterGame = ::realmmesh::protocol::edge::v1::EnterGame;
using EnterGameAccepted = ::realmmesh::protocol::edge::v1::EnterGameAccepted;
using EdgeAttach = ::realmmesh::protocol::edge::v1::EdgeAttach;
using EdgeAttachAccepted = ::realmmesh::protocol::edge::v1::EdgeAttachAccepted;
using EnterRealmGranted = ::realmmesh::protocol::edge::v1::EnterRealmGranted;
using EdgeError = ::realmmesh::protocol::edge::v1::EdgeError;
using EdgeMessageId = ::realmmesh::protocol::edge::v1::MessageId;
using EdgeTransportProtocol =
    ::realmmesh::protocol::edge::v1::TransportProtocol;

inline constexpr std::uint32_t kEdgeProtocolVersion = 1;

/// EdgeError.code 的取值(主 spec §5.1:1xxx 沿用 edge.proto 凭据段、
/// 2xxx 排队段):1001 身份凭据无效、1004 网关满额拒绝 attach(#43)、
/// 2001 号牌无效(排队段;与旧链路 realm login ticket 同码,按消息
/// 上下文区分,#50 退役旧链路后收敛)、2002 未认证、3001 旧链路
/// EnterGame 票据无效。
inline constexpr int edge_error_invalid_credentials = 1001;
inline constexpr int edge_error_attach_out_of_budget = 1004;
inline constexpr int edge_error_invalid_login_ticket = 2001;
inline constexpr int edge_error_invalid_queue_number = 2001;
inline constexpr int edge_error_not_authenticated = 2002;
inline constexpr int edge_error_invalid_enter_game_ticket = 3001;

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
    const HeartbeatRequest& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const HeartbeatResponse& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterGame& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterGameAccepted& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EdgeAttach& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EdgeAttachAccepted& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterRealmGranted& message, std::uint64_t request_id = 0);
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
[[nodiscard]] std::optional<HeartbeatRequest> decode_heartbeat_request(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<HeartbeatResponse> decode_heartbeat_response(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterGame> decode_enter_game(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterGameAccepted> decode_enter_game_accepted(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EdgeAttach> decode_edge_attach(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EdgeAttachAccepted> decode_edge_attach_accepted(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterRealmGranted> decode_enter_realm_granted(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EdgeError> decode_edge_error(
    std::span<const std::byte> payload);

}  // namespace realm::game::common
