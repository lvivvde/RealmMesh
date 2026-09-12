#pragma once

#include "realmmesh/game/common/compact_jws.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace realm::game::common {

/// 身份 Token 的 claims 集(ADR-0004)。schema 完全受控:头与载荷的键集、
/// 值类型一一枚举,validate 拒绝任何多余、缺失或类型不符的成员。
struct IdentityClaims {
    std::string issuer;  // iss,与验签方的期望值比较
    std::uint64_t account_id;  // sub,十进制字符串承载(边缘 JS 2^53 之上仍精确)
    std::string jti;  // 32 字符小写 hex
    std::chrono::system_clock::time_point issued_at;  // iat
    std::chrono::system_clock::time_point expires_at;  // exp
};

/// 身份 Token(EdDSA 紧凑 JWS)的编解码:签发与验证同址,claims schema
/// 与时效规则(jws_clock_leeway)只活在这一处。JWS 机制(密钥/签名/受控
/// 头)由 CompactJws 承载;自建而非引库的取舍见
/// docs/adr/0004-identity-token-jwt-eddsa.md。
class IdentityTokenCodec final {
public:
    IdentityTokenCodec(Ed25519Seed seed, std::string kid);

    /// 签发紧缩三段式 JWS(R‖S 签名,base64url 无填充)。
    /// 前提:claims 由本方签发路径构造(jti 为 32 字符小写 hex、
    /// issuer 非空、expires_at 不早于 issued_at)。
    [[nodiscard]] std::string issue(const IdentityClaims& claims) const;

    /// 验签在 claims 语义之前;头/载荷逐键核对,签名不合法即整体拒绝。
    /// account_id 为 0 视为非法(先例同 SessionTicketCodec)。
    [[nodiscard]] std::optional<IdentityClaims> validate(
        std::string_view token,
        std::string_view expected_issuer,
        std::chrono::system_clock::time_point now) const;

    /// `{"keys":[{kty:"OKP",crv:"Ed25519",x,kid}]}`,供可编程边缘拉取。
    [[nodiscard]] std::string jwks() const;

private:
    CompactJws jws_;
    std::string kid_;
};

/// 身份 Token jti 的单次消费守卫(spec #43 网关入口;先例同
/// TicketReplayGuard):条目随 exp 过期清理,重放返回 false。
/// 非线程安全:网关业务帧线程单线程约定。
class IdentityReplayGuard final {
public:
    [[nodiscard]] bool consume(
        const IdentityClaims& claims,
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now());

private:
    std::unordered_map<std::string, std::chrono::system_clock::time_point>
        consumed_;
};

/// 64 个 hex 字符(大小写均可)→ 32 字节种子;长度或字符不合规即抛出。

}  // namespace realm::game::common
