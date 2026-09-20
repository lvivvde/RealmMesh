#include "realmmesh/game/common/queue_number_v2.hpp"

#include "realmmesh/game/common/json.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace realm::game::common {
namespace {

constexpr std::size_t identity_jti_size = 32;

bool valid_jti(std::string_view value) {
    return value.size() == identity_jti_size &&
        std::ranges::all_of(value, [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

bool valid_kid(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
        std::ranges::all_of(value, [](char character) {
            return (character >= 'a' && character <= 'z') ||
                (character >= 'A' && character <= 'Z') ||
                (character >= '0' && character <= '9') ||
                character == '-' || character == '_' || character == '.';
        });
}

std::int64_t epoch_seconds(std::chrono::system_clock::time_point value) {
    return std::chrono::duration_cast<std::chrono::seconds>(
               value.time_since_epoch())
        .count();
}

}  // namespace

QueueNumberV2Codec::QueueNumberV2Codec(
    QueueNumberV2SigningKey active_signing_key,
    std::vector<QueueNumberV2VerificationKey> verification_keys,
    std::chrono::seconds ttl)
    : signer_(active_signing_key.seed),
      active_kid_(std::move(active_signing_key.kid)),
      ttl_(ttl) {
    if (!valid_kid(active_kid_)) {
        throw std::invalid_argument("invalid Queue Number signing kid");
    }
    if (ttl_ <= std::chrono::seconds::zero() ||
        ttl_ > queue_number_v2_max_ttl) {
        throw std::invalid_argument("Queue Number TTL is outside protocol bounds");
    }
    if (verification_keys.empty()) {
        throw std::invalid_argument("Queue Number verification ring is empty");
    }

    bool active_present = false;
    const auto active_public = ed25519_public_key_from_seed(active_signing_key.seed);
    for (auto& key : verification_keys) {
        if (!valid_kid(key.kid)) {
            throw std::invalid_argument("invalid Queue Number verification kid");
        }
        if (key.kid == active_kid_) {
            if (key.public_key != active_public) {
                throw std::invalid_argument(
                    "Queue Number active kid does not match signing key");
            }
            active_present = true;
        }
        auto [ignored, inserted] = verification_keys_.emplace(
            std::move(key.kid), CompactJwsVerifier(key.public_key));
        static_cast<void>(ignored);
        if (!inserted) {
            throw std::invalid_argument("duplicate Queue Number verification kid");
        }
    }
    if (!active_present) {
        throw std::invalid_argument(
            "Queue Number verification ring omits active signing key");
    }
}

std::string QueueNumberV2Codec::issue(const QueueNumberV2Issue& input) const {
    if (!valid_jti(input.identity_jti) || input.number == 0 ||
        input.number > static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max()) ||
        input.identity_expires_at < input.issued_at) {
        throw std::invalid_argument("invalid Queue Number v2 issue input");
    }
    const auto expires_at =
        std::min(input.identity_expires_at, input.issued_at + ttl_);
    const JsonObject header{
        {"alg", std::string{"EdDSA"}},
        {"typ", std::string{"JWT"}},
        {"kid", active_kid_},
    };
    const JsonObject payload{
        {"version", queue_number_v2_version},
        {"iss", std::string{queue_number_v2_issuer}},
        {"aud", std::string{queue_number_v2_audience}},
        {"purpose", std::string{queue_number_v2_purpose}},
        {"identity_jti", input.identity_jti},
        {"number", static_cast<std::int64_t>(input.number)},
        {"iat", epoch_seconds(input.issued_at)},
        {"exp", epoch_seconds(expires_at)},
    };
    return signer_.encode(header, payload);
}

std::optional<QueueNumberV2Claims> QueueNumberV2Codec::validate(
    std::string_view token,
    std::chrono::system_clock::time_point now) const {
    const auto kid = compact_jws_key_id(token);
    if (!kid.has_value()) return std::nullopt;
    const auto selected = verification_keys_.find(*kid);
    if (selected == verification_keys_.end()) return std::nullopt;
    const auto payload = selected->second.decode(token, *kid);
    if (!payload.has_value()) return std::nullopt;

    const auto* version = json_int_member(*payload, "version");
    const auto* issuer = json_string_member(*payload, "iss");
    const auto* audience = json_string_member(*payload, "aud");
    const auto* purpose = json_string_member(*payload, "purpose");
    const auto* identity_jti = json_string_member(*payload, "identity_jti");
    const auto* number = json_int_member(*payload, "number");
    const auto* issued_at = json_int_member(*payload, "iat");
    const auto* expires_at = json_int_member(*payload, "exp");
    if (payload->size() != 8 || version == nullptr || issuer == nullptr ||
        audience == nullptr || purpose == nullptr || identity_jti == nullptr ||
        number == nullptr || issued_at == nullptr || expires_at == nullptr) {
        return std::nullopt;
    }
    if (*version != queue_number_v2_version ||
        *issuer != queue_number_v2_issuer ||
        *audience != queue_number_v2_audience ||
        *purpose != queue_number_v2_purpose ||
        !valid_jti(*identity_jti) ||
        *number <= 0) {
        return std::nullopt;
    }

    const auto issued = std::chrono::system_clock::time_point{
        std::chrono::seconds{*issued_at}};
    const auto expires = std::chrono::system_clock::time_point{
        std::chrono::seconds{*expires_at}};
    if (expires < issued || expires > issued + ttl_ ||
        now > expires + jws_clock_leeway ||
        now + jws_clock_leeway < issued) {
        return std::nullopt;
    }
    return QueueNumberV2Claims{
        .identity_jti = *identity_jti,
        .number = static_cast<std::uint64_t>(*number),
        .issued_at = issued,
        .expires_at = expires,
    };
}

std::optional<QueueNumberV2Claims> QueueNumberV2Codec::validate(
    std::string_view token,
    std::string_view expected_identity_jti,
    std::chrono::system_clock::time_point identity_expires_at,
    std::chrono::system_clock::time_point now) const {
    if (!valid_jti(expected_identity_jti)) return std::nullopt;
    auto claims = validate(token, now);
    if (!claims.has_value() ||
        claims->identity_jti != expected_identity_jti ||
        claims->expires_at > identity_expires_at) {
        return std::nullopt;
    }
    return claims;
}

}  // namespace realm::game::common
