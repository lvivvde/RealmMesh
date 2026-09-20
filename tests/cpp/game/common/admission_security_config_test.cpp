#include "realmmesh/game/common/admission_security_config.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace realm::game::common {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view queue_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view grant_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view old_queue_seed_hex =
    "c5aa8df43f9f837bedb7442f31dcb7b166d38535076f094b85ce3a2e0b4458f7";

QueueAdmissionSecurityConfig valid_config() {
    const auto queue_seed = parse_identity_seed_hex(queue_seed_hex);
    const auto grant_seed = parse_identity_seed_hex(grant_seed_hex);
    return {
        .active_queue_number_key = QueueNumberV2SigningKey{
            .kid = "queue-v2", .seed = queue_seed},
        .queue_number_verification_keys = {
            {.kid = "queue-v2",
             .public_key = ed25519_public_key_from_seed(queue_seed)}},
        .active_admission_grant_key = AdmissionGrantSigningKey{
            .kid = "grant-v1", .seed = grant_seed},
        .admission_grant_verification_keys = {
            {.kid = "grant-v1",
             .public_key = ed25519_public_key_from_seed(grant_seed)}},
        .queue_number_ttl = 1h,
        .admission_grant = {
            .issuer = "realmmesh/queue",
            .deployment_id = "prod-shanghai",
            .grant_window = 5min,
        },
    };
}

TEST(AdmissionSecurityConfigTest, AcceptsSeparatedRolesAndRotationRings) {
    auto config = valid_config();
    const auto old_queue_seed = parse_identity_seed_hex(old_queue_seed_hex);
    // Overlap is legal inside one role when kid and key remain unique.
    config.queue_number_verification_keys.push_back(
        {.kid = "queue-v1",
         .public_key = ed25519_public_key_from_seed(old_queue_seed)});
    EXPECT_NO_THROW(config.validate());
}

TEST(AdmissionSecurityConfigTest, RejectsMissingActiveSigningRole) {
    auto config = valid_config();
    config.active_admission_grant_key.reset();
    EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(AdmissionSecurityConfigTest, RejectsDuplicateAndUnknownActiveKeys) {
    auto duplicate = valid_config();
    duplicate.admission_grant_verification_keys.push_back(
        duplicate.admission_grant_verification_keys.front());
    EXPECT_THROW(duplicate.validate(), std::invalid_argument);

    auto missing = valid_config();
    missing.queue_number_verification_keys.front().kid = "queue-old";
    EXPECT_THROW(missing.validate(), std::invalid_argument);

    auto missing_grant = valid_config();
    missing_grant.admission_grant_verification_keys.front().kid = "grant-old";
    EXPECT_THROW(missing_grant.validate(), std::invalid_argument);
}

TEST(AdmissionSecurityConfigTest, RejectsRoleConfusedKidOrSigningMaterial) {
    auto same_kid = valid_config();
    same_kid.active_admission_grant_key->kid = "queue-v2";
    EXPECT_THROW(same_kid.validate(), std::invalid_argument);

    auto same_seed = valid_config();
    same_seed.active_admission_grant_key->seed =
        same_seed.active_queue_number_key->seed;
    same_seed.admission_grant_verification_keys.front().public_key =
        same_seed.queue_number_verification_keys.front().public_key;
    EXPECT_THROW(same_seed.validate(), std::invalid_argument);
}

TEST(AdmissionSecurityConfigTest, RejectsUnsafeBoundsAndIdentifiers) {
    auto bad_ttl = valid_config();
    bad_ttl.queue_number_ttl = 0s;
    EXPECT_THROW(bad_ttl.validate(), std::invalid_argument);

    auto bad_window = valid_config();
    bad_window.admission_grant.grant_window = 0s;
    EXPECT_THROW(bad_window.validate(), std::invalid_argument);

    auto bad_issuer = valid_config();
    bad_issuer.admission_grant.issuer.clear();
    EXPECT_THROW(bad_issuer.validate(), std::invalid_argument);

    auto bad_deployment = valid_config();
    bad_deployment.admission_grant.deployment_id = "has whitespace";
    EXPECT_THROW(bad_deployment.validate(), std::invalid_argument);
}

TEST(AdmissionSecurityConfigTest, ParsesPublicKeysStrictly) {
    const auto seed = parse_identity_seed_hex(queue_seed_hex);
    const auto public_key = ed25519_public_key_from_seed(seed);
    EXPECT_EQ(public_key.bytes.size(), 32U);
    EXPECT_THROW(
        static_cast<void>(parse_ed25519_public_key_hex("00")),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(
            parse_ed25519_public_key_hex(std::string(64, '0'))),
        std::invalid_argument);

    EXPECT_THROW(
        CompactJwsVerifier(Ed25519PublicKey{}), std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::common
