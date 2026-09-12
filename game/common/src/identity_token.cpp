#include "realmmesh/game/common/identity_token.hpp"

#include <sodium.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

#include "realmmesh/game/common/base64url.hpp"
#include "realmmesh/game/common/json.hpp"
#include "hex.hpp"

namespace realm::game::common {
namespace {

// 受控常量:受众固定本服务族(RFC 8725 §3.9),用途固定访问链路。
constexpr std::string_view audience = "realmmesh-access";
constexpr std::string_view purpose = "access";
constexpr std::size_t signature_size = crypto_sign_BYTES;  // Ed25519 裸 R‖S
constexpr std::size_t jti_size = 32;
constexpr auto clock_leeway = std::chrono::seconds{60};

std::span<const std::byte> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string_view as_string_view(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool is_lowercase_hex(char value) {
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
}

const std::string* string_member(
    const JsonObject& object,
    std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        return nullptr;
    }
    return std::get_if<std::string>(&found->second);
}

const std::int64_t* int_member(const JsonObject& object, std::string_view key) {
    const auto found = object.find(std::string{key});
    if (found == object.end()) {
        return nullptr;
    }
    return std::get_if<std::int64_t>(&found->second);
}

}  // namespace

IdentityTokenCodec::IdentityTokenCodec(Ed25519Seed seed, std::string kid)
    : kid_(std::move(kid)) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    crypto_sign_seed_keypair(
        reinterpret_cast<unsigned char*>(public_key_.data()),
        reinterpret_cast<unsigned char*>(secret_key_.data()),
        reinterpret_cast<const unsigned char*>(seed.data()));
}

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

    const auto signing_input =
        base64url_encode(as_bytes(JsonCodec::encode(header))) + "." +
        base64url_encode(as_bytes(JsonCodec::encode(payload)));
    unsigned char signature[crypto_sign_BYTES];
    unsigned long long signature_length = 0;
    crypto_sign_detached(
        signature,
        &signature_length,
        reinterpret_cast<const unsigned char*>(signing_input.data()),
        signing_input.size(),
        reinterpret_cast<const unsigned char*>(secret_key_.data()));
    return signing_input + "." +
        base64url_encode(std::as_bytes(std::span{signature}));
}

std::optional<IdentityClaims> IdentityTokenCodec::validate(
    std::string_view token,
    std::string_view expected_issuer,
    std::chrono::system_clock::time_point now) const {
    const auto first = token.find('.');
    const auto second =
        first == std::string_view::npos ? first : token.find('.', first + 1);
    if (second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;  // 恰好三段:头.载荷.签名。
    }
    const auto header_bytes =
        base64url_decode(token.substr(0, first));
    const auto payload_bytes =
        base64url_decode(token.substr(first + 1, second - first - 1));
    const auto signature_bytes = base64url_decode(token.substr(second + 1));
    if (!header_bytes || !payload_bytes || !signature_bytes ||
        signature_bytes->size() != signature_size) {
        return std::nullopt;
    }

    const auto header = JsonCodec::decode(as_string_view(*header_bytes));
    if (!header.has_value()) {
        return std::nullopt;
    }
    const auto* alg = string_member(*header, "alg");
    const auto* typ = string_member(*header, "typ");
    const auto* kid = string_member(*header, "kid");
    if (header->size() != 3 || alg == nullptr || *alg != "EdDSA" ||
        typ == nullptr || *typ != "JWT" || kid == nullptr || *kid != kid_) {
        return std::nullopt;
    }

    // 先验签,后做 claims 语义:任何载荷内容都改变不了判定路径。
    const auto verified = crypto_sign_verify_detached(
        reinterpret_cast<const unsigned char*>(signature_bytes->data()),
        reinterpret_cast<const unsigned char*>(token.data()),
        second,
        reinterpret_cast<const unsigned char*>(public_key_.data()));
    if (verified != 0) {
        return std::nullopt;
    }

    const auto payload = JsonCodec::decode(as_string_view(*payload_bytes));
    if (!payload.has_value()) {
        return std::nullopt;
    }
    const auto* issuer = string_member(*payload, "iss");
    const auto* sub = string_member(*payload, "sub");
    const auto* audience_member = string_member(*payload, "aud");
    const auto* purpose_member = string_member(*payload, "purpose");
    const auto* jti = string_member(*payload, "jti");
    const auto* issued_at = int_member(*payload, "iat");
    const auto* expires_at = int_member(*payload, "exp");
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
        !std::ranges::all_of(*jti, is_lowercase_hex)) {
        return std::nullopt;
    }

    const auto issued =
        std::chrono::system_clock::time_point(std::chrono::seconds{*issued_at});
    const auto expires = std::chrono::system_clock::time_point(
        std::chrono::seconds{*expires_at});
    if (now > expires + clock_leeway || now + clock_leeway < issued) {
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
        {"x", base64url_encode(public_key_)},
        {"kid", kid_},
    };
    return R"({"keys":[)" + JsonCodec::encode(jwk) + "]}";
}

Ed25519Seed parse_identity_seed_hex(std::string_view value) {
    if (value.size() != ed25519_seed_size * 2) {
        throw std::invalid_argument(
            "identity seed must contain 64 hex characters");
    }
    Ed25519Seed seed{};
    for (std::size_t index = 0; index < seed.size(); ++index) {
        const int high = hex::nibble(value[index * 2]);
        const int low = hex::nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("identity seed contains non-hex data");
        }
        seed[index] = static_cast<std::byte>((high << 4) | low);
    }
    return seed;
}

}  // namespace realm::game::common
