#pragma once

#include "realmmesh/game/common/compact_jws.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace realm::game::common {

/// 排队号牌签发方(ADR-0006);与身份 Token 的 iss(realmmesh/login-verify)
/// 区分凭据类型。
inline constexpr std::string_view queue_number_issuer = "realmmesh/queue";

/// 排队号牌的 claims 集(ADR-0006):号值 + admitted。schema 受控——无
/// sub/jti/aud(号牌不单次消费,位次查询复用,断线凭它找回位次)。
struct QueueNumberClaims {
    std::uint64_t number;  // 号值,1 起
    bool admitted;  // 放行凭证 = 重签 admitted=true
    std::chrono::system_clock::time_point issued_at;  // iat
    std::chrono::system_clock::time_point expires_at;  // exp
};

/// 排队号牌(EdDSA 紧凑 JWS)的编解码:签发与验证同址,claims schema 与
/// 时效规则(jws_clock_leeway)只活在这一处;JWS 机制由 CompactJws 承载。
class QueueNumberCodec final {
public:
    QueueNumberCodec(Ed25519Seed seed, std::string kid);

    /// 签发紧缩三段式 JWS;前提:claims 由本方签发路径构造
    /// (number ≥ 1、expires_at 不早于 issued_at)。
    [[nodiscard]] std::string issue(const QueueNumberClaims& claims) const;

    /// 验签在 claims 语义之前;头/载荷逐键核对,签名不合法即整体拒绝。
    /// number 为 0 视为非法(先例同 IdentityTokenCodec 的 account_id)。
    [[nodiscard]] std::optional<QueueNumberClaims> validate(
        std::string_view token,
        std::chrono::system_clock::time_point now) const;

private:
    CompactJws jws_;
    std::string kid_;
};

}  // namespace realm::game::common
