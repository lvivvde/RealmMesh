#pragma once

#include "realmmesh/game/common/json.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

namespace realm::game::common {

inline constexpr std::size_t ed25519_seed_size = 32;
/// Ed25519 私钥以 32 字节种子承载(RFC 8037 §7.1),公钥可由种子确定导出。
using Ed25519Seed = std::array<std::byte, ed25519_seed_size>;

/// JWT 时效判定的共享时钟容差(各凭据编解码同一取值)。
inline constexpr std::chrono::seconds jws_clock_leeway{60};

/// EdDSA 紧凑 JWS 的签发与验签核心(ADR-0004):密钥派生、detached
/// 签名、三段式解析与受控头(alg/typ/kid)逐键校验。claims schema 与
/// 时效规则由各凭据编解码自持——本核心只回答"这段签名是否出自本键
/// 且头是受控形态"。
class CompactJws final {
public:
    explicit CompactJws(Ed25519Seed seed);

    /// 受控头 + 载荷 → 紧缩三段式(base64url 无填充,R‖S 签名)。
    [[nodiscard]] std::string encode(
        const JsonObject& header, const JsonObject& payload) const;

    /// 三段式解析 → 头逐键校验(alg=EdDSA/typ=JWT/kid 一致、无多余
    /// 成员)→ Ed25519 验签;返回已验签的载荷 JSON。任何一步不满足
    /// 返回 std::nullopt,拒绝是确定性的。
    [[nodiscard]] std::optional<JsonObject> decode(
        std::string_view token, std::string_view expected_kid) const;

    /// 32 字节原始公钥(JWKS 的 x 参数承载方)。
    [[nodiscard]] const std::array<std::byte, 32>& public_key() const noexcept;

private:
    std::array<std::byte, 32> public_key_{};
    std::array<std::byte, 64> secret_key_{};
};

/// 64 个 hex 字符(大小写均可)→ 32 字节种子;长度或字符不合规即抛出。
[[nodiscard]] Ed25519Seed parse_identity_seed_hex(std::string_view value);

}  // namespace realm::game::common
