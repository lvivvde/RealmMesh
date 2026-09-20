#pragma once

#include "realmmesh/game/common/compact_jws.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realm::game::common {

inline constexpr std::int64_t admission_grant_version = 1;
inline constexpr std::string_view admission_grant_audience =
    "realmmesh-gateway";
inline constexpr std::string_view admission_grant_purpose =
    "gateway-admission";
inline constexpr std::chrono::seconds admission_grant_max_window{600};

struct AdmissionGrantSigningKey final {
    std::string kid;
    Ed25519Seed seed;
};

struct AdmissionGrantVerificationKey final {
    std::string kid;
    Ed25519PublicKey public_key;
};

struct AdmissionGrantPolicy final {
    std::string issuer;
    std::string deployment_id;
    std::chrono::seconds grant_window{300};

    void validate() const;
};

struct AdmissionGrantIssue final {
    std::string grant_jti;
    std::string identity_jti;
    std::uint64_t queue_number{0};
    std::chrono::system_clock::time_point released_at;
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point identity_expires_at;
};

struct AdmissionGrantClaims final {
    std::string issuer;
    std::string grant_jti;
    std::string identity_jti;
    std::uint64_t queue_number{0};
    std::string deployment_id;
    std::chrono::system_clock::time_point released_at;
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point expires_at;
};

/// Queue Scheduler 专用签发模块，只持一把活动私钥。
class AdmissionGrantIssuer final {
public:
    AdmissionGrantIssuer(
        AdmissionGrantSigningKey active_signing_key,
        AdmissionGrantPolicy policy);

    [[nodiscard]] std::string issue(const AdmissionGrantIssue& input) const;

private:
    CompactJws signer_;
    std::string active_kid_;
    AdmissionGrantPolicy policy_;
};

/// Gateway 专用验证模块，只持 kid 索引的公钥环，不暴露签发能力。
class AdmissionGrantVerifier final {
public:
    AdmissionGrantVerifier(
        std::vector<AdmissionGrantVerificationKey> verification_keys,
        AdmissionGrantPolicy policy);

    [[nodiscard]] std::optional<AdmissionGrantClaims> validate(
        std::string_view token,
        std::string_view expected_identity_jti,
        std::chrono::system_clock::time_point identity_expires_at,
        std::chrono::system_clock::time_point now) const;

private:
    std::unordered_map<std::string, CompactJwsVerifier> verification_keys_;
    AdmissionGrantPolicy policy_;
};

}  // namespace realm::game::common
