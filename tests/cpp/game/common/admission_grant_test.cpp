#include "realmmesh/game/common/admission_grant.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "realmmesh/game/common/json.hpp"
#include "realmmesh/game/common/queue_number_v2.hpp"

namespace realm::game::common {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view grant_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view queue_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view identity_jti = "0123456789abcdef0123456789abcdef";
constexpr std::string_view grant_jti = "abcdef0123456789abcdef0123456789";

std::chrono::system_clock::time_point at(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

AdmissionGrantPolicy policy(std::string deployment = "prod-shanghai") {
    return {
        .issuer = "realmmesh/queue",
        .deployment_id = std::move(deployment),
        .grant_window = 300s,
    };
}

AdmissionGrantSigningKey signing_key(
    std::string kid = "grant-v1",
    std::string_view seed_hex = grant_seed_hex) {
    return {.kid = std::move(kid),
            .seed = parse_identity_seed_hex(seed_hex)};
}

AdmissionGrantVerificationKey verification_key(
    std::string kid = "grant-v1",
    std::string_view seed_hex = grant_seed_hex) {
    const auto seed = parse_identity_seed_hex(seed_hex);
    return {.kid = std::move(kid),
            .public_key = ed25519_public_key_from_seed(seed)};
}

AdmissionGrantIssue issue_input() {
    return {
        .grant_jti = std::string{grant_jti},
        .identity_jti = std::string{identity_jti},
        .queue_number = 42,
        .released_at = at(1'700'000'000),
        .issued_at = at(1'700'000'010),
        .identity_expires_at = at(1'700'003'600),
    };
}

AdmissionGrantIssuer issuer() {
    return AdmissionGrantIssuer(signing_key(), policy());
}

AdmissionGrantVerifier verifier() {
    return AdmissionGrantVerifier({verification_key()}, policy());
}

JsonObject valid_payload() {
    return {
        {"version", admission_grant_version},
        {"iss", std::string{"realmmesh/queue"}},
        {"aud", std::string{admission_grant_audience}},
        {"purpose", std::string{admission_grant_purpose}},
        {"grant_jti", std::string{grant_jti}},
        {"identity_jti", std::string{identity_jti}},
        {"queue_number", std::int64_t{42}},
        {"deployment_id", std::string{"prod-shanghai"}},
        {"released_at", std::int64_t{1'700'000'000}},
        {"iat", std::int64_t{1'700'000'010}},
        {"exp", std::int64_t{1'700'000'300}},
    };
}

std::string sign_payload(
    const JsonObject& payload,
    std::string kid = "grant-v1",
    std::string_view seed_hex = grant_seed_hex) {
    return CompactJws(parse_identity_seed_hex(seed_hex)).encode(
        {{"alg", std::string{"EdDSA"}},
         {"typ", std::string{"JWT"}},
         {"kid", std::move(kid)}},
        payload);
}

TEST(AdmissionGrantTest, RoundTripsStrictDeploymentBoundSchema) {
    const auto token = issuer().issue(issue_input());
    const auto claims = verifier().validate(
        token, identity_jti, issue_input().identity_expires_at,
        at(1'700'000'100));
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->issuer, "realmmesh/queue");
    EXPECT_EQ(claims->grant_jti, grant_jti);
    EXPECT_EQ(claims->identity_jti, identity_jti);
    EXPECT_EQ(claims->queue_number, 42U);
    EXPECT_EQ(claims->deployment_id, "prod-shanghai");
    EXPECT_EQ(claims->released_at, issue_input().released_at);
    EXPECT_EQ(claims->issued_at, issue_input().issued_at);
    EXPECT_EQ(claims->expires_at, at(1'700'000'300));
}

TEST(AdmissionGrantTest, CapsExpiryAtBoundIdentityExpiry) {
    auto input = issue_input();
    input.identity_expires_at = at(1'700'000'120);
    const auto claims = verifier().validate(
        issuer().issue(input), identity_jti, input.identity_expires_at,
        input.issued_at);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->expires_at, input.identity_expires_at);
}

TEST(AdmissionGrantTest, RejectsWrongDeploymentIssuerBindingAndUnknownKid) {
    const auto token = issuer().issue(issue_input());
    EXPECT_FALSE(AdmissionGrantVerifier(
                     {verification_key()}, policy("prod-europe"))
                     .validate(token, identity_jti,
                               issue_input().identity_expires_at,
                               at(1'700'000'100))
                     .has_value());
    auto wrong_issuer = policy();
    wrong_issuer.issuer = "attacker";
    EXPECT_FALSE(AdmissionGrantVerifier({verification_key()}, wrong_issuer)
                     .validate(token, identity_jti,
                               issue_input().identity_expires_at,
                               at(1'700'000'100))
                     .has_value());
    EXPECT_FALSE(verifier().validate(
        token, "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        issue_input().identity_expires_at, at(1'700'000'100)).has_value());

    const auto unknown = AdmissionGrantIssuer(
        signing_key("grant-unknown"), policy()).issue(issue_input());
    EXPECT_FALSE(verifier().validate(
        unknown, identity_jti, issue_input().identity_expires_at,
        at(1'700'000'100)).has_value());
}

TEST(AdmissionGrantTest, RejectsStrictSchemaAndTimeWindowViolations) {
    const auto validate = [](const JsonObject& payload) {
        return verifier().validate(
            sign_payload(payload), identity_jti, at(1'700'003'600),
            at(1'700'000'100));
    };

    auto extra = valid_payload();
    extra.emplace("admitted", true);
    EXPECT_FALSE(validate(extra).has_value());
    auto missing = valid_payload();
    missing.erase("grant_jti");
    EXPECT_FALSE(validate(missing).has_value());
    auto wrong_version = valid_payload();
    wrong_version["version"] = std::int64_t{2};
    EXPECT_FALSE(validate(wrong_version).has_value());
    auto wrong_audience = valid_payload();
    wrong_audience["aud"] = std::string{"realmmesh-queue"};
    EXPECT_FALSE(validate(wrong_audience).has_value());
    auto wrong_purpose = valid_payload();
    wrong_purpose["purpose"] = std::string{"queue-position"};
    EXPECT_FALSE(validate(wrong_purpose).has_value());
    auto wrong_type = valid_payload();
    wrong_type["queue_number"] = std::string{"42"};
    EXPECT_FALSE(validate(wrong_type).has_value());
    auto renewed = valid_payload();
    renewed["exp"] = std::int64_t{1'700'000'301};
    EXPECT_FALSE(validate(renewed).has_value());
    auto future = valid_payload();
    future["iat"] = std::int64_t{1'700'000'301};
    EXPECT_FALSE(validate(future).has_value());
}

TEST(AdmissionGrantTest, AcceptsConfiguredRotationOverlap) {
    const AdmissionGrantIssuer old_issuer(
        signing_key("grant-old", queue_seed_hex), policy());
    const auto old_token = old_issuer.issue(issue_input());
    const AdmissionGrantVerifier rotated(
        {verification_key("grant-old", queue_seed_hex),
         verification_key("grant-v1", grant_seed_hex)},
        policy());
    EXPECT_TRUE(rotated.validate(
        old_token, identity_jti, issue_input().identity_expires_at,
        at(1'700'000'100)).has_value());
}

TEST(AdmissionGrantTest, RejectsExpiredFutureAndMalformedTokens) {
    const auto token = issuer().issue(issue_input());
    EXPECT_FALSE(verifier().validate(
        token, identity_jti, issue_input().identity_expires_at,
        at(1'700'000'300) + jws_clock_leeway + 1s).has_value());
    EXPECT_FALSE(verifier().validate(
        token, identity_jti, issue_input().identity_expires_at,
        issue_input().issued_at - jws_clock_leeway - 1s).has_value());
    EXPECT_FALSE(verifier().validate(
        "a.b.c", identity_jti, issue_input().identity_expires_at,
        at(1'700'000'100)).has_value());
}

TEST(AdmissionGrantTest, QueueNumberCannotValidateAsGrant) {
    const auto queue_seed = parse_identity_seed_hex(queue_seed_hex);
    const QueueNumberV2Codec queue_codec(
        {.kid = "queue-v2", .seed = queue_seed},
        {{.kid = "queue-v2",
          .public_key = ed25519_public_key_from_seed(queue_seed)}},
        1h);
    const QueueNumberV2Issue queue_issue{
        .identity_jti = std::string{identity_jti},
        .number = 42,
        .issued_at = at(1'700'000'000),
        .identity_expires_at = at(1'700'003'600),
    };
    const auto token = queue_codec.issue(queue_issue);
    EXPECT_FALSE(verifier().validate(
        token, identity_jti, queue_issue.identity_expires_at,
        at(1'700'000'100)).has_value());
}

TEST(AdmissionGrantTest, GrantCannotValidateAsQueueNumberEvenWithSharedKey) {
    const auto seed = parse_identity_seed_hex(grant_seed_hex);
    const auto token = issuer().issue(issue_input());
    const QueueNumberV2Codec confused(
        {.kid = "grant-v1", .seed = seed},
        {{.kid = "grant-v1", .public_key = ed25519_public_key_from_seed(seed)}},
        1h);
    EXPECT_FALSE(confused.validate(
        token, identity_jti, issue_input().identity_expires_at,
        at(1'700'000'100)).has_value());
}

TEST(AdmissionGrantTest, RejectsUnsafePolicyRingAndTimes) {
    EXPECT_THROW(
        AdmissionGrantVerifier({}, policy()), std::invalid_argument);
    auto unsafe = policy();
    unsafe.grant_window = admission_grant_max_window + 1s;
    EXPECT_THROW(AdmissionGrantIssuer(signing_key(), unsafe),
                 std::invalid_argument);
    EXPECT_THROW(
        AdmissionGrantVerifier(
            {verification_key(), verification_key()}, policy()),
        std::invalid_argument);
    auto invalid = issue_input();
    invalid.issued_at = invalid.released_at - 1s;
    EXPECT_THROW(
        static_cast<void>(issuer().issue(invalid)), std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::common
