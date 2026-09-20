#include "realmmesh/game/queue/queue_ticketing.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <string>

namespace realm::game::queue {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view queue_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";
constexpr std::string_view grant_seed_hex =
    "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb";
constexpr std::string_view identity_jti =
    "0123456789abcdef0123456789abcdef";

std::chrono::system_clock::time_point at(std::int64_t seconds) {
    return std::chrono::system_clock::time_point{std::chrono::seconds{seconds}};
}

common::QueueNumberV2Codec number_codec() {
    const auto seed = common::parse_identity_seed_hex(queue_seed_hex);
    return common::QueueNumberV2Codec(
        {.kid = "queue-v2", .seed = seed},
        {{.kid = "queue-v2",
          .public_key = common::ed25519_public_key_from_seed(seed)}},
        1h);
}

common::AdmissionGrantIssuer grant_issuer() {
    return common::AdmissionGrantIssuer(
        {.kid = "grant-v1",
         .seed = common::parse_identity_seed_hex(grant_seed_hex)},
        {.issuer = "realmmesh/queue",
         .deployment_id = "prod-shanghai",
         .grant_window = 5min});
}

common::AdmissionGrantVerifier grant_verifier() {
    const auto seed = common::parse_identity_seed_hex(grant_seed_hex);
    return common::AdmissionGrantVerifier(
        {{.kid = "grant-v1",
          .public_key = common::ed25519_public_key_from_seed(seed)}},
        {.issuer = "realmmesh/queue",
         .deployment_id = "prod-shanghai",
         .grant_window = 5min});
}

common::IdentityClaims identity(
    std::chrono::system_clock::time_point expires_at = at(4'000)) {
    return {
        .issuer = "realmmesh/login-verify",
        .account_id = 42,
        .jti = std::string{identity_jti},
        .issued_at = at(0),
        .expires_at = expires_at,
    };
}

TEST(QueueTicketingTest, IssuesIdentityBoundQueueNumberV2) {
    QueueCore core(100);
    QueueTicketing ticketing(core, number_codec(), grant_issuer());
    const auto issued = ticketing.issue(identity(at(90)), at(10));

    EXPECT_EQ(issued.number, 1U);
    const auto claims = number_codec().validate(issued.queue_number_token, at(11));
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->identity_jti, identity_jti);
    EXPECT_EQ(claims->number, 1U);
    EXPECT_EQ(claims->expires_at, at(90));
}

TEST(QueueTicketingTest, ReturnsQueuedShapeBeforeRelease) {
    QueueCore core(100);
    QueueTicketing ticketing(core, number_codec(), grant_issuer());
    const auto issued = ticketing.issue(identity(), at(10));

    const auto result = ticketing.query(issued.queue_number_token, at(20));
    EXPECT_EQ(result.status, QueueTicketQueryStatus::Queued);
    EXPECT_EQ(result.number, 1U);
    EXPECT_EQ(result.position, 1U);
    EXPECT_FALSE(result.admission_grant.has_value());
}

TEST(QueueTicketingTest, IdempotentIssueAfterReleaseDoesNotUnderflowEta) {
    QueueCore core(100);
    QueueTicketing ticketing(core, number_codec(), grant_issuer());
    const auto first = ticketing.issue(identity(), at(10));
    ASSERT_EQ(
        core.release_batch(BudgetAggregate{100, 100}, at(100)), 1U);

    const auto replay = ticketing.issue(identity(), at(110));
    EXPECT_EQ(replay.number, first.number);
    EXPECT_EQ(replay.estimated_wait_seconds, 0U);
}

TEST(QueueTicketingTest, RepeatedQueriesReturnByteIdenticalGrant) {
    QueueCore core(100, 10s, 30min, 100, 5min);
    QueueTicketing ticketing(core, number_codec(), grant_issuer());
    const auto issued = ticketing.issue(identity(), at(10));
    ASSERT_EQ(
        core.release_batch(BudgetAggregate{100, 100}, at(100)), 1U);

    const auto first = ticketing.query(issued.queue_number_token, at(110));
    const auto second = ticketing.query(issued.queue_number_token, at(340));
    ASSERT_EQ(first.status, QueueTicketQueryStatus::Admitted);
    ASSERT_EQ(second.status, QueueTicketQueryStatus::Admitted);
    ASSERT_TRUE(first.admission_grant.has_value());
    ASSERT_TRUE(second.admission_grant.has_value());
    EXPECT_EQ(*first.admission_grant, *second.admission_grant);

    const auto claims = grant_verifier().validate(
        *first.admission_grant, identity_jti, at(4'000), at(110));
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->queue_number, 1U);
    EXPECT_EQ(claims->released_at, at(100));
    EXPECT_EQ(claims->issued_at, at(100));
    EXPECT_EQ(claims->expires_at, at(400));
}

TEST(QueueTicketingTest, RestartKeepsOriginalGrantAndReleaseWindow) {
    QueueCore source(100, 10s, 30min, 100, 5min);
    QueueTicketing before(source, number_codec(), grant_issuer());
    const auto issued = before.issue(identity(), at(10));
    ASSERT_EQ(
        source.release_batch(BudgetAggregate{100, 100}, at(100)), 1U);
    const auto first = before.query(issued.queue_number_token, at(120));
    ASSERT_TRUE(first.admission_grant.has_value());

    QueueCore restored(100, 10s, 30min, 100, 5min);
    restored.restore(source.snapshot(at(120)), at(200));
    QueueTicketing after(restored, number_codec(), grant_issuer());
    const auto second = after.query(issued.queue_number_token, at(200));
    ASSERT_TRUE(second.admission_grant.has_value());
    EXPECT_EQ(*first.admission_grant, *second.admission_grant);
}

TEST(QueueTicketingTest, ReturnsExplicitExpiredAndInvalidOutcomes) {
    QueueCore core(100, 10s, 30min, 100, 5min);
    QueueTicketing ticketing(core, number_codec(), grant_issuer());
    const auto issued = ticketing.issue(identity(), at(10));
    ASSERT_EQ(
        core.release_batch(BudgetAggregate{100, 100}, at(100)), 1U);

    EXPECT_EQ(
        ticketing.query(issued.queue_number_token, at(461)).status,
        QueueTicketQueryStatus::ReleaseExpired);
    EXPECT_EQ(
        ticketing.query("not-a-token", at(110)).status,
        QueueTicketQueryStatus::InvalidQueueNumber);
}

}  // namespace
}  // namespace realm::game::queue
