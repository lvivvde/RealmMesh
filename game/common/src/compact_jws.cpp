#include "realmmesh/game/common/compact_jws.hpp"

#include <sodium.h>

#include <cstdlib>
#include <stdexcept>
#include <utility>

#include "realmmesh/game/common/base64url.hpp"
#include "hex.hpp"

namespace realm::game::common {
namespace {

constexpr std::size_t signature_size = crypto_sign_BYTES;  // Ed25519 裸 R‖S

std::span<const std::byte> as_bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string_view as_string_view(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

std::optional<JsonObject> decode_with_public_key(
    std::string_view token,
    std::string_view expected_kid,
    const Ed25519PublicKey& public_key) {
    const auto first = token.find('.');
    const auto second =
        first == std::string_view::npos ? first : token.find('.', first + 1);
    if (second == std::string_view::npos ||
        token.find('.', second + 1) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto header_bytes = base64url_decode(token.substr(0, first));
    const auto payload_bytes =
        base64url_decode(token.substr(first + 1, second - first - 1));
    const auto signature_bytes = base64url_decode(token.substr(second + 1));
    if (!header_bytes || !payload_bytes || !signature_bytes ||
        signature_bytes->size() != signature_size) {
        return std::nullopt;
    }

    const auto header = JsonCodec::decode(as_string_view(*header_bytes));
    if (!header.has_value()) return std::nullopt;
    const auto* alg = json_string_member(*header, "alg");
    const auto* typ = json_string_member(*header, "typ");
    const auto* kid = json_string_member(*header, "kid");
    if (header->size() != 3 || alg == nullptr || *alg != "EdDSA" ||
        typ == nullptr || *typ != "JWT" || kid == nullptr ||
        *kid != expected_kid) {
        return std::nullopt;
    }

    const auto verified = crypto_sign_verify_detached(
        reinterpret_cast<const unsigned char*>(signature_bytes->data()),
        reinterpret_cast<const unsigned char*>(token.data()),
        second,
        reinterpret_cast<const unsigned char*>(public_key.bytes.data()));
    if (verified != 0) return std::nullopt;
    return JsonCodec::decode(as_string_view(*payload_bytes));
}

Ed25519PublicKey derive_public_key(const Ed25519Seed& seed) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    Ed25519PublicKey public_key;
    std::array<std::byte, 64> secret_key{};
    crypto_sign_seed_keypair(
        reinterpret_cast<unsigned char*>(public_key.bytes.data()),
        reinterpret_cast<unsigned char*>(secret_key.data()),
        reinterpret_cast<const unsigned char*>(seed.data()));
    sodium_memzero(secret_key.data(), secret_key.size());
    return public_key;
}

}  // namespace

Ed25519Seed seed_from_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string{name} + " is not set");
    }
    return parse_identity_seed_hex(value);
}

CompactJws::CompactJws(Ed25519Seed seed) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    crypto_sign_seed_keypair(
        reinterpret_cast<unsigned char*>(public_key_.data()),
        reinterpret_cast<unsigned char*>(secret_key_.data()),
        reinterpret_cast<const unsigned char*>(seed.data()));
}

std::string CompactJws::encode(
    const JsonObject& header, const JsonObject& payload) const {
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

std::optional<JsonObject> CompactJws::decode(
    std::string_view token, std::string_view expected_kid) const {
    return decode_with_public_key(
        token, expected_kid, Ed25519PublicKey{public_key_});
}

const std::array<std::byte, 32>& CompactJws::public_key() const noexcept {
    return public_key_;
}

CompactJwsVerifier::CompactJwsVerifier(Ed25519PublicKey public_key)
    : public_key_(std::move(public_key)) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    if (crypto_core_ed25519_is_valid_point(
            reinterpret_cast<const unsigned char*>(
                public_key_.bytes.data())) != 1) {
        throw std::invalid_argument("invalid Ed25519 public key");
    }
}

std::optional<JsonObject> CompactJwsVerifier::decode(
    std::string_view token, std::string_view expected_kid) const {
    return decode_with_public_key(token, expected_kid, public_key_);
}

std::optional<std::string> compact_jws_key_id(std::string_view token) {
    const auto first = token.find('.');
    if (first == std::string_view::npos) return std::nullopt;
    const auto header_bytes = base64url_decode(token.substr(0, first));
    if (!header_bytes.has_value()) return std::nullopt;
    const auto header = JsonCodec::decode(as_string_view(*header_bytes));
    if (!header.has_value()) return std::nullopt;
    const auto* alg = json_string_member(*header, "alg");
    const auto* typ = json_string_member(*header, "typ");
    const auto* kid = json_string_member(*header, "kid");
    if (header->size() != 3 || alg == nullptr || *alg != "EdDSA" ||
        typ == nullptr || *typ != "JWT" || kid == nullptr || kid->empty()) {
        return std::nullopt;
    }
    return *kid;
}

Ed25519PublicKey ed25519_public_key_from_seed(const Ed25519Seed& seed) {
    return derive_public_key(seed);
}

Ed25519PublicKey parse_ed25519_public_key_hex(std::string_view value) {
    if (value.size() != ed25519_seed_size * 2) {
        throw std::invalid_argument(
            "Ed25519 public key must contain 64 hex characters");
    }
    Ed25519PublicKey key;
    for (std::size_t index = 0; index < key.bytes.size(); ++index) {
        const int high = hex::nibble(value[index * 2]);
        const int low = hex::nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument(
                "Ed25519 public key contains non-hex data");
        }
        key.bytes[index] = static_cast<std::byte>((high << 4) | low);
    }
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    if (crypto_core_ed25519_is_valid_point(
            reinterpret_cast<const unsigned char*>(key.bytes.data())) != 1) {
        throw std::invalid_argument("invalid Ed25519 public key");
    }
    return key;
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
