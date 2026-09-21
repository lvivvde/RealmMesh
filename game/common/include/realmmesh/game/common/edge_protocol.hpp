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
using HeartbeatRequest = ::realmmesh::protocol::edge::v1::HeartbeatRequest;
using HeartbeatResponse = ::realmmesh::protocol::edge::v1::HeartbeatResponse;
using EdgeAttach = ::realmmesh::protocol::edge::v1::EdgeAttach;
using EdgeAttachAccepted = ::realmmesh::protocol::edge::v1::EdgeAttachAccepted;
using EnterRealmGranted = ::realmmesh::protocol::edge::v1::EnterRealmGranted;
using EnterRealm = ::realmmesh::protocol::edge::v1::EnterRealm;
using EnterRealmAccepted = ::realmmesh::protocol::edge::v1::EnterRealmAccepted;
using EdgeError = ::realmmesh::protocol::edge::v1::EdgeError;
using EdgeMessageId = ::realmmesh::protocol::edge::v1::MessageId;
using EdgeTransportProtocol =
    ::realmmesh::protocol::edge::v1::TransportProtocol;

inline constexpr std::uint32_t kEdgeProtocolVersion = 1;

/// EdgeError.code 的取值(主 spec §5.1:1xxx 沿用 edge.proto 凭据段、
/// 2xxx 排队段):1001 身份凭据无效、1004 网关满额拒绝 attach(#43)、
/// 2001 号牌无效(排队段)、2002 未认证、3002 EnterRealm 直连票据无效
/// (#46)。
///
/// 两个错误码不再分配:2001 曾兼作 realm 登录票据无效(与排队号牌同
/// 码,按消息上下文区分),3001 为 EnterGame 入场票据无效。
inline constexpr int edge_error_invalid_credentials = 1001;
inline constexpr int edge_error_attach_out_of_budget = 1004;
inline constexpr int edge_error_admission_in_progress = 1005;
inline constexpr int edge_error_admission_unavailable = 1006;
inline constexpr int edge_error_throttled = 429;
inline constexpr int edge_error_invalid_queue_number = 2001;
inline constexpr int edge_error_not_authenticated = 2002;
inline constexpr int edge_error_invalid_enter_realm_ticket = 3002;

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
    const HeartbeatRequest& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const HeartbeatResponse& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EdgeAttach& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EdgeAttachAccepted& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterRealmGranted& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterRealm& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterRealmAccepted& message, std::uint64_t request_id = 0);
[[nodiscard]] std::vector<std::byte> encode(
    const EdgeError& message, std::uint64_t request_id = 0);

[[nodiscard]] std::optional<HeartbeatRequest> decode_heartbeat_request(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<HeartbeatResponse> decode_heartbeat_response(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EdgeAttach> decode_edge_attach(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EdgeAttachAccepted> decode_edge_attach_accepted(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterRealmGranted> decode_enter_realm_granted(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterRealm> decode_enter_realm(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EnterRealmAccepted> decode_enter_realm_accepted(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<EdgeError> decode_edge_error(
    std::span<const std::byte> payload);

}  // namespace realm::game::common
