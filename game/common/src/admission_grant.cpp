#include "realmmesh/game/common/admission_grant.hpp"

#include "realmmesh/game/common/json.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <utility>

namespace realm::game::common {
namespace {

constexpr std::size_t jti_size = 32;

bool valid_jti(std::string_view value) {
    return value.size() == jti_size &&
        std::ranges::all_of(value, [](char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f');
        });
}

bool valid_identifier(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum &&
        std::ranges::all_of(value, [](char character) {
            return character >= '!' && character <= '~';
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

void AdmissionGrantPolicy::validate() const {
    if (!valid_identifier(issuer, 128) ||
        !valid_identifier(deployment_id, 128)) {
        throw std::invalid_argument(
            "Admission Grant issuer and deployment must be bounded identifiers");
    }
    if (grant_window <= std::chrono::seconds::zero() ||
        grant_window > admission_grant_max_window) {
        throw std::invalid_argument(
            "Admission Grant window is outside protocol bounds");
    }
}

AdmissionGrantIssuer::AdmissionGrantIssuer(
    AdmissionGrantSigningKey active_signing_key,
    AdmissionGrantPolicy policy)
    : signer_(active_signing_key.seed),
      active_kid_(std::move(active_signing_key.kid)),
      policy_(std::move(policy)) {
    policy_.validate();
    if (!valid_kid(active_kid_)) {
        throw std::invalid_argument("invalid Admission Grant signing kid");
    }
}

std::string AdmissionGrantIssuer::issue(
    const AdmissionGrantIssue& input) const {
    if (!valid_jti(input.grant_jti) || !valid_jti(input.identity_jti) ||
        input.queue_number == 0 ||
        input.queue_number > static_cast<std::uint64_t>(
                                 std::numeric_limits<std::int64_t>::max()) ||
        input.issued_at < input.released_at) {
        throw std::invalid_argument("invalid Admission Grant issue input");
    }
    const auto expires_at =
        std::min(input.identity_expires_at,
                 input.released_at + policy_.grant_window);
    if (expires_at < input.issued_at) {
        throw std::invalid_argument(
            "Admission Grant cannot outlive its identity or release window");
    }

    const JsonObject header{
        {"alg", std::string{"EdDSA"}},
        {"typ", std::string{"JWT"}},
        {"kid", active_kid_},
    };
    const JsonObject payload{
        {"version", admission_grant_version},
        {"iss", policy_.issuer},
        {"aud", std::string{admission_grant_audience}},
        {"purpose", std::string{admission_grant_purpose}},
        {"grant_jti", input.grant_jti},
        {"identity_jti", input.identity_jti},
        {"queue_number", static_cast<std::int64_t>(input.queue_number)},
        {"deployment_id", policy_.deployment_id},
        {"released_at", epoch_seconds(input.released_at)},
        {"iat", epoch_seconds(input.issued_at)},
        {"exp", epoch_seconds(expires_at)},
    };
    return signer_.encode(header, payload);
}

AdmissionGrantVerifier::AdmissionGrantVerifier(
    std::vector<AdmissionGrantVerificationKey> verification_keys,
    AdmissionGrantPolicy policy)
    : policy_(std::move(policy)) {
    policy_.validate();
    if (verification_keys.empty()) {
        throw std::invalid_argument("Admission Grant verification ring is empty");
    }
    for (auto& key : verification_keys) {
        if (!valid_kid(key.kid)) {
            throw std::invalid_argument("invalid Admission Grant verification kid");
        }
        auto [ignored, inserted] = verification_keys_.emplace(
            std::move(key.kid), CompactJwsVerifier(key.public_key));
        static_cast<void>(ignored);
        if (!inserted) {
            throw std::invalid_argument(
                "duplicate Admission Grant verification kid");
        }
    }
}

std::optional<AdmissionGrantClaims> AdmissionGrantVerifier::validate(
    std::string_view token,
    std::string_view expected_identity_jti,
    std::chrono::system_clock::time_point identity_expires_at,
    std::chrono::system_clock::time_point now) const {
    if (!valid_jti(expected_identity_jti)) return std::nullopt;
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
    const auto* grant_jti = json_string_member(*payload, "grant_jti");
    const auto* identity_jti = json_string_member(*payload, "identity_jti");
    const auto* queue_number = json_int_member(*payload, "queue_number");
    const auto* deployment_id = json_string_member(*payload, "deployment_id");
    const auto* released_at = json_int_member(*payload, "released_at");
    const auto* issued_at = json_int_member(*payload, "iat");
    const auto* expires_at = json_int_member(*payload, "exp");
    if (payload->size() != 11 || version == nullptr || issuer == nullptr ||
        audience == nullptr || purpose == nullptr || grant_jti == nullptr ||
        identity_jti == nullptr || queue_number == nullptr ||
        deployment_id == nullptr || released_at == nullptr ||
        issued_at == nullptr || expires_at == nullptr) {
        return std::nullopt;
    }
    if (*version != admission_grant_version || *issuer != policy_.issuer ||
        *audience != admission_grant_audience ||
        *purpose != admission_grant_purpose ||
        *identity_jti != expected_identity_jti || !valid_jti(*identity_jti) ||
        !valid_jti(*grant_jti) || *queue_number <= 0 ||
        *deployment_id != policy_.deployment_id) {
        return std::nullopt;
    }

    const auto released = std::chrono::system_clock::time_point{
        std::chrono::seconds{*released_at}};
    const auto issued = std::chrono::system_clock::time_point{
        std::chrono::seconds{*issued_at}};
    const auto expires = std::chrono::system_clock::time_point{
        std::chrono::seconds{*expires_at}};
    if (issued < released || expires < issued ||
        expires > released + policy_.grant_window ||
        expires > identity_expires_at ||
        now > expires + jws_clock_leeway ||
        now + jws_clock_leeway < issued) {
        return std::nullopt;
    }
    return AdmissionGrantClaims{
        .issuer = *issuer,
        .grant_jti = *grant_jti,
        .identity_jti = *identity_jti,
        .queue_number = static_cast<std::uint64_t>(*queue_number),
        .deployment_id = *deployment_id,
        .released_at = released,
        .issued_at = issued,
        .expires_at = expires,
    };
}

}  // namespace realm::game::common
