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
/// 2xxx 排队段):1001 凭据无效(签名/schema/绑定/部署/时效/已消费一律
/// 收敛于此,见 docs/adr/0009)、1004 网关满额拒绝 attach(#43)、
/// 1005 准入进行中、1006 准入存储不可用、1007 账号不具备准入资格(仅指
/// 封禁或不在白名单,终态,不重试;角色不再参与准入,ADR-0013)、1008 玩家
/// 数据暂不可用(拉取重试耗尽,Grant 已消费,客户端重走 Login Verifier,
/// #98)、429 限流(Realm 侧亦指数据容量满,带 retry_after_seconds)、
/// 2002 未认证、3002 EnterRealm 直连票据无效(#46)。
///
/// 3003-3010 为 Realm Session 业务错误(#93),回包后会话保持:3003 阶段
/// 不符、3004 角色名非法、3005 角色名已占用、3006 角色数已满、3007 角色
/// 不属于本账号或本 Realm、3008 已满级、3009 训练序号非法、3010 玩家数据
/// 暂不可用(可重试)。
///
/// 三个号码不再分配:2001 曾兼作排队号牌无效(排队号牌不再进入 Gateway;
/// 该语义只活在 Queue 的 HTTPS 错误模型里,与 Edge 错误码分属两个编号
/// 空间)、realm 登录票据无效、3001 EnterGame 入场票据无效。
inline constexpr int edge_error_invalid_credentials = 1001;
inline constexpr int edge_error_attach_out_of_budget = 1004;
inline constexpr int edge_error_admission_in_progress = 1005;
inline constexpr int edge_error_admission_unavailable = 1006;
inline constexpr int edge_error_not_eligible = 1007;
inline constexpr int edge_error_player_data_unavailable = 1008;
inline constexpr int edge_error_throttled = 429;
inline constexpr int edge_error_not_authenticated = 2002;
inline constexpr int edge_error_invalid_enter_realm_ticket = 3002;
inline constexpr int edge_error_phase_mismatch = 3003;
inline constexpr int edge_error_invalid_character_name = 3004;
inline constexpr int edge_error_character_name_taken = 3005;
inline constexpr int edge_error_character_limit_reached = 3006;
inline constexpr int edge_error_character_not_owned = 3007;
inline constexpr int edge_error_max_level = 3008;
inline constexpr int edge_error_invalid_training_seq = 3009;
inline constexpr int edge_error_realm_data_unavailable = 3010;

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
