#include "realmmesh/game/common/queue_number.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace realm::game::common {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

std::chrono::system_clock::time_point at_time(std::int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds{seconds});
}

QueueNumberCodec test_codec() {
    return QueueNumberCodec(parse_identity_seed_hex(seed_hex), "test-kid");
}

QueueNumberClaims test_claims() {
    return QueueNumberClaims{
        .number = 42,
        .admitted = false,
        .issued_at = at_time(1'700'000'000),
        .expires_at = at_time(1'700'000'000) + 3600s,
    };
}

TEST(QueueNumberCodecTest, RoundTripsQueuedTicket) {
    const QueueNumberCodec codec = test_codec();
    const auto token = codec.issue(test_claims());
    const auto claims = codec.validate(token, at_time(1'700'000'100));
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->number, 42U);
    EXPECT_FALSE(claims->admitted);
    EXPECT_EQ(claims->issued_at, test_claims().issued_at);
    EXPECT_EQ(claims->expires_at, test_claims().expires_at);
}

TEST(QueueNumberCodecTest, RoundTripsAdmittedTicket) {
    const QueueNumberCodec codec = test_codec();
    auto claims = test_claims();
    claims.admitted = true;
    claims.expires_at = claims.issued_at + 300s;
    const auto decoded = codec.validate(codec.issue(claims), claims.issued_at + 1s);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->admitted);
    EXPECT_EQ(decoded->number, 42U);
}

TEST(QueueNumberCodecTest, AcceptsWithinLeewayAfterExpiry) {
    const QueueNumberCodec codec = test_codec();
    const auto token = codec.issue(test_claims());
    // 容差边界内(60s)仍有效,超出即拒绝。
    const auto claims = test_claims();
    EXPECT_TRUE(
        codec.validate(token, claims.expires_at + common::jws_clock_leeway).has_value());
    EXPECT_FALSE(
        codec.validate(token, claims.expires_at + common::jws_clock_leeway + 1s)
            .has_value());
}

TEST(QueueNumberCodecTest, RejectsTokenFromFuture) {
    const QueueNumberCodec codec = test_codec();
    const auto token = codec.issue(test_claims());
    EXPECT_FALSE(
        codec.validate(token, test_claims().issued_at - common::jws_clock_leeway - 1s)
            .has_value());
}

TEST(QueueNumberCodecTest, RejectsWrongKid) {
    const QueueNumberCodec codec =
        QueueNumberCodec(common::parse_identity_seed_hex(seed_hex), "other-kid");
    const auto token = codec.issue(test_claims());
    EXPECT_FALSE(
        test_codec().validate(token, at_time(1'700'000'100)).has_value());
}

TEST(QueueNumberCodecTest, RejectsZeroNumber) {
    const QueueNumberCodec codec = test_codec();
    auto claims = test_claims();
    claims.number = 0;
    const auto token = codec.issue(claims);
    EXPECT_FALSE(codec.validate(token, at_time(1'700'000'100)).has_value());
}

TEST(QueueNumberCodecTest, RejectsGarbage) {
    const QueueNumberCodec codec = test_codec();
    EXPECT_FALSE(codec.validate("", at_time(1'700'000'100)).has_value());
    EXPECT_FALSE(
        codec.validate("a.b.c", at_time(1'700'000'100)).has_value());
}

}  // namespace
}  // namespace realm::game::common
