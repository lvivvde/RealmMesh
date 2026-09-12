#include "realmmesh/game/common/identity_token.hpp"

#include <algorithm>
#include <limits>

#include "realmmesh/game/common/base64url.hpp"
#include "realmmesh/game/common/json.hpp"

namespace realm::game::common {
namespace {

// 受控常量:受众固定本服务族(RFC 8725 §3.9),用途固定访问链路。
constexpr std::string_view audience = "realmmesh-access";
constexpr std::string_view purpose = "access";
constexpr std::size_t jti_size = 32;

}  // namespace

IdentityTokenCodec::IdentityTokenCodec(Ed25519Seed seed, std::string kid)
    : jws_(seed),
      kid_(std::move(kid)) {}

std::string IdentityTokenCodec::issue(const IdentityClaims& claims) const {
    const JsonObject header{
        {"alg", std::string{"EdDSA"}},
        {"typ", std::string{"JWT"}},
        {"kid", kid_},
    };
    const auto issued_at = std::chrono::duration_cast<std::chrono::seconds>(
                               claims.issued_at.time_since_epoch())
                               .count();
    const auto expires_at = std::chrono::duration_cast<std::chrono::seconds>(
                                claims.expires_at.time_since_epoch())
                                .count();
    const JsonObject payload{
        {"iss", claims.issuer},
        {"sub", std::to_string(claims.account_id)},
        {"aud", std::string{audience}},
        {"purpose", std::string{purpose}},
        {"iat", static_cast<std::int64_t>(issued_at)},
        {"exp", static_cast<std::int64_t>(expires_at)},
        {"jti", claims.jti},
    };
    return jws_.encode(header, payload);
}

std::optional<IdentityClaims> IdentityTokenCodec::validate(
    std::string_view token,
    std::string_view expected_issuer,
    std::chrono::system_clock::time_point now) const {
    const auto payload = jws_.decode(token, kid_);
    if (!payload.has_value()) {
        return std::nullopt;
    }

    const auto* issuer = json_string_member(*payload, "iss");
    const auto* sub = json_string_member(*payload, "sub");
    const auto* audience_member = json_string_member(*payload, "aud");
    const auto* purpose_member = json_string_member(*payload, "purpose");
    const auto* jti = json_string_member(*payload, "jti");
    const auto* issued_at = json_int_member(*payload, "iat");
    const auto* expires_at = json_int_member(*payload, "exp");
    if (payload->size() != 7 || issuer == nullptr || sub == nullptr ||
        audience_member == nullptr || purpose_member == nullptr ||
        jti == nullptr || issued_at == nullptr || expires_at == nullptr) {
        return std::nullopt;
    }
    if (*issuer != expected_issuer || *audience_member != audience ||
        *purpose_member != purpose) {
        return std::nullopt;
    }

    if (sub->empty() || sub->size() > 20 ||
        (sub->size() > 1 && (*sub)[0] == '0')) {
        return std::nullopt;  // 只认规范十进制:无前导零、无符号。
    }
    std::uint64_t account_id = 0;
    for (const char digit : *sub) {
        if (digit < '0' || digit > '9') {
            return std::nullopt;
        }
        const auto value = static_cast<std::uint64_t>(digit - '0');
        if (account_id >
            (std::numeric_limits<std::uint64_t>::max() - value) / 10U) {
            return std::nullopt;
        }
        account_id = account_id * 10U + value;
    }
    if (account_id == 0 || jti->size() != jti_size ||
        !std::ranges::all_of(*jti, [](char value) {
            return (value >= '0' && value <= '9') ||
                (value >= 'a' && value <= 'f');
        })) {
        return std::nullopt;
    }

    const auto issued =
        std::chrono::system_clock::time_point(std::chrono::seconds{*issued_at});
    const auto expires = std::chrono::system_clock::time_point(
        std::chrono::seconds{*expires_at});
    if (now > expires + jws_clock_leeway || now + jws_clock_leeway < issued) {
        return std::nullopt;
    }

    return IdentityClaims{
        .issuer = *issuer,
        .account_id = account_id,
        .jti = *jti,
        .issued_at = issued,
        .expires_at = expires,
    };
}

std::string IdentityTokenCodec::jwks() const {
    // JsonCodec 只编扁平对象,外层 keys 数组只能字符串拼装;
    // 内层 JWK 对象仍走同一条受控编码路径。
    const JsonObject jwk{
        {"kty", std::string{"OKP"}},
        {"crv", std::string{"Ed25519"}},
        {"x", base64url_encode(jws_.public_key())},
        {"kid", kid_},
    };
    return R"({"keys":[)" + JsonCodec::encode(jwk) + "]}";
}

}  // namespace realm::game::common
