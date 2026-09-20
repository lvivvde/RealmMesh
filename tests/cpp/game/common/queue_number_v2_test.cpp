#include "realmmesh/game/common/queue_number_v2.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <vector>

#include "realmmesh/game/common/base64url.hpp"
#include "realmmesh/game/common/json.hpp"

namespace realm::game::common {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view seed_one_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view seed_two_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view identity_jti = "0123456789abcdef0123456789abcdef";

std::chrono::system_clock::time_point at(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

QueueNumberV2SigningKey signing_key(
    std::string kid = "queue-v2",
    std::string_view seed_hex = seed_one_hex) {
    return {.kid = std::move(kid), .seed = parse_identity_seed_hex(seed_hex)};
}

QueueNumberV2VerificationKey verification_key(
    std::string kid = "queue-v2",
    std::string_view seed_hex = seed_one_hex) {
    const auto seed = parse_identity_seed_hex(seed_hex);
    return {.kid = std::move(kid),
            .public_key = ed25519_public_key_from_seed(seed)};
}

QueueNumberV2Codec codec() {
    return QueueNumberV2Codec(
        signing_key(), {verification_key()}, std::chrono::hours{1});
}

QueueNumberV2Issue issue_input() {
    return {
        .identity_jti = std::string{identity_jti},
        .number = 42,
        .issued_at = at(1'700'000'000),
        .identity_expires_at = at(1'700'007'200),
    };
}

JsonObject valid_payload() {
    return {
        {"version", queue_number_v2_version},
        {"iss", std::string{queue_number_v2_issuer}},
        {"aud", std::string{queue_number_v2_audience}},
        {"purpose", std::string{queue_number_v2_purpose}},
        {"identity_jti", std::string{identity_jti}},
        {"number", std::int64_t{42}},
        {"iat", std::int64_t{1'700'000'000}},
        {"exp", std::int64_t{1'700'003'600}},
    };
}

std::string sign_payload(
    const JsonObject& payload,
    std::string kid = "queue-v2",
    std::string_view seed_hex = seed_one_hex) {
    return CompactJws(parse_identity_seed_hex(seed_hex)).encode(
        {{"alg", std::string{"EdDSA"}},
         {"typ", std::string{"JWT"}},
         {"kid", std::move(kid)}},
        payload);
}

TEST(QueueNumberV2CodecTest, RoundTripsStrictIdentityBoundSchema) {
    const auto token = codec().issue(issue_input());
    const auto claims = codec().validate(
        token, identity_jti, issue_input().identity_expires_at,
        at(1'700'000'100));
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->identity_jti, identity_jti);
    EXPECT_EQ(claims->number, 42U);
    EXPECT_EQ(claims->issued_at, issue_input().issued_at);
    EXPECT_EQ(claims->expires_at, at(1'700'003'600));

    const auto first = token.find('.');
    const auto second = token.find('.', first + 1);
    const auto bytes = base64url_decode(
        std::string_view{token}.substr(first + 1, second - first - 1));
    ASSERT_TRUE(bytes.has_value());
    const auto payload = JsonCodec::decode(std::string_view{
        reinterpret_cast<const char*>(bytes->data()), bytes->size()});
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(payload->size(), 8U);
    EXPECT_EQ(payload->find("admitted"), payload->end());
}

TEST(QueueNumberV2CodecTest, CapsExpiryAtBoundIdentityExpiry) {
    auto input = issue_input();
    input.identity_expires_at = input.issued_at + 90s;
    const auto claims = codec().validate(
        codec().issue(input), identity_jti, input.identity_expires_at,
        input.issued_at + 1s);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->expires_at, input.identity_expires_at);
}

TEST(QueueNumberV2CodecTest, RejectsSchemaAudiencePurposeAndBindingChanges) {
    const auto validate = [](const JsonObject& payload) {
        return codec().validate(
            sign_payload(payload), identity_jti, at(1'700'007'200),
            at(1'700'000'100));
    };

    auto extra = valid_payload();
    extra.emplace("admitted", false);
    EXPECT_FALSE(validate(extra).has_value());
    auto missing = valid_payload();
    missing.erase("purpose");
    EXPECT_FALSE(validate(missing).has_value());
    auto wrong_version = valid_payload();
    wrong_version["version"] = std::int64_t{1};
    EXPECT_FALSE(validate(wrong_version).has_value());
    auto wrong_audience = valid_payload();
    wrong_audience["aud"] = std::string{"realmmesh-gateway"};
    EXPECT_FALSE(validate(wrong_audience).has_value());
    auto wrong_purpose = valid_payload();
    wrong_purpose["purpose"] = std::string{"gateway-admission"};
    EXPECT_FALSE(validate(wrong_purpose).has_value());
    auto wrong_binding = valid_payload();
    wrong_binding["identity_jti"] =
        std::string{"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
    EXPECT_FALSE(validate(wrong_binding).has_value());
    auto wrong_type = valid_payload();
    wrong_type["number"] = std::string{"42"};
    EXPECT_FALSE(validate(wrong_type).has_value());
}

TEST(QueueNumberV2CodecTest, SelectsKnownKidAndAcceptsRotationOverlap) {
    const QueueNumberV2Codec old_codec(
        signing_key("queue-old", seed_one_hex),
        {verification_key("queue-old", seed_one_hex)}, 1h);
    const auto old_token = old_codec.issue(issue_input());
    const QueueNumberV2Codec rotated(
        signing_key("queue-new", seed_two_hex),
        {verification_key("queue-old", seed_one_hex),
         verification_key("queue-new", seed_two_hex)},
        1h);
    EXPECT_TRUE(rotated.validate(
        old_token, identity_jti, issue_input().identity_expires_at,
        at(1'700'000'100)).has_value());
    EXPECT_FALSE(rotated.validate(
        sign_payload(valid_payload(), "queue-unknown"), identity_jti,
        issue_input().identity_expires_at, at(1'700'000'100)).has_value());
}

TEST(QueueNumberV2CodecTest, RejectsExpiredFutureAndMalformedTokens) {
    const auto token = codec().issue(issue_input());
    EXPECT_FALSE(codec().validate(
        token, identity_jti, issue_input().identity_expires_at,
        at(1'700'003'600) + jws_clock_leeway + 1s).has_value());
    EXPECT_FALSE(codec().validate(
        token, identity_jti, issue_input().identity_expires_at,
        issue_input().issued_at - jws_clock_leeway - 1s).has_value());
    EXPECT_FALSE(codec().validate(
        "a.b.c", identity_jti, issue_input().identity_expires_at,
        at(1'700'000'100)).has_value());
}

TEST(QueueNumberV2CodecTest, RejectsUnsafeConstructionAndIssueInputs) {
    EXPECT_THROW(
        QueueNumberV2Codec(signing_key(), {}, 1h), std::invalid_argument);
    EXPECT_THROW(
        QueueNumberV2Codec(signing_key(), {verification_key()}, 0s),
        std::invalid_argument);
    EXPECT_THROW(
        QueueNumberV2Codec(
            signing_key(), {verification_key()},
            queue_number_v2_max_ttl + 1s),
        std::invalid_argument);
    EXPECT_THROW(
        QueueNumberV2Codec(
            signing_key(), {verification_key(), verification_key()}, 1h),
        std::invalid_argument);
    auto invalid = issue_input();
    invalid.identity_jti = "not-a-jti";
    EXPECT_THROW(
        static_cast<void>(codec().issue(invalid)), std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::common
