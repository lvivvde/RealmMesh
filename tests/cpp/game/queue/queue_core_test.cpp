#include "realmmesh/game/queue/queue_core.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace realm::game::queue {
namespace {

using namespace std::chrono_literals;

using TimePoint = std::chrono::system_clock::time_point;

TimePoint at_time(std::int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds{seconds});
}

TEST(QueueCoreTest, IssuesSequentialNumbers) {
    QueueCore core(100);
    EXPECT_EQ(core.issue("jti-a", at_time(0)), (QueueCore::Issued{1, true}));
    EXPECT_EQ(core.issue("jti-b", at_time(0)), (QueueCore::Issued{2, true}));
    EXPECT_EQ(core.issue("jti-c", at_time(0)), (QueueCore::Issued{3, true}));
    EXPECT_EQ(core.next_number(), 4U);
}

TEST(QueueCoreTest, SameJtiReplaysSameNumber) {
    QueueCore core(100);
    EXPECT_EQ(core.issue("jti-a", at_time(0)), (QueueCore::Issued{1, true}));
    EXPECT_EQ(core.issue("jti-a", at_time(5)), (QueueCore::Issued{1, false}));
    EXPECT_EQ(core.next_number(), 2U);
    EXPECT_EQ(core.issue("jti-b", at_time(5)), (QueueCore::Issued{2, true}));
}

TEST(QueueCoreTest, IdempotencyEntriesExpireByTtl) {
    QueueCore core(100, 10s, 30s, 1);
    EXPECT_EQ(core.issue("jti-a", at_time(0)), (QueueCore::Issued{1, true}));
    // 未过期:超限仍插入(保护是尽力而为,发号不可失败)。
    EXPECT_EQ(core.issue("jti-b", at_time(1)), (QueueCore::Issued{2, true}));
    // 过期后重插同 jti = 新号(其身份会话早已失效)。
    EXPECT_EQ(core.issue("jti-a", at_time(31)), (QueueCore::Issued{3, true}));
}

TEST(QueueCoreTest, ReleaseBatchHonorsStepAndBudgets) {
    QueueCore core(100);
    for (int index = 0; index < 250; ++index) {
        static_cast<void>(core.issue("jti-" + std::to_string(index), at_time(0)));
    }
    const BudgetAggregate budgets{500, 500};
    EXPECT_EQ(core.release_batch(budgets, at_time(0)), 100U);
    EXPECT_EQ(core.released_number(), 100U);
    EXPECT_EQ(core.release_batch(budgets, at_time(2)), 100U);
    EXPECT_EQ(core.release_batch(budgets, at_time(4)), 50U);
    EXPECT_EQ(core.release_batch(budgets, at_time(6)), 0U);
}

TEST(QueueCoreTest, ReleaseLedgerMapsEveryBatchBoundaryToStableTime) {
    QueueCore core(3);
    for (int index = 0; index < 6; ++index) {
        static_cast<void>(core.issue("jti-" + std::to_string(index), at_time(0)));
    }
    const BudgetAggregate budgets{10, 10};
    EXPECT_EQ(core.release_batch(budgets, at_time(100)), 3U);
    EXPECT_EQ(core.release_batch(budgets, at_time(102)), 3U);

    for (const auto number : {1U, 2U, 3U}) {
        EXPECT_EQ(
            core.release_eligibility(number, at_time(200)),
            (QueueReleaseEligibility{
                QueueReleaseStatus::Eligible, at_time(100)}));
    }
    for (const auto number : {4U, 5U, 6U}) {
        EXPECT_EQ(
            core.release_eligibility(number, at_time(200)),
            (QueueReleaseEligibility{
                QueueReleaseStatus::Eligible, at_time(102)}));
    }
    EXPECT_EQ(
        core.release_eligibility(7, at_time(200)).status,
        QueueReleaseStatus::NotReleased);
}

TEST(QueueCoreTest, ReleaseTimeIsNormalizedToPersistedJwsPrecision) {
    QueueCore core(1);
    static_cast<void>(core.issue("jti-a", at_time(0)));
    ASSERT_EQ(
        core.release_batch(
            BudgetAggregate{1, 1}, at_time(100) + 750ms),
        1U);
    const auto eligibility = core.release_eligibility(1, at_time(101));
    EXPECT_EQ(eligibility.status, QueueReleaseStatus::Eligible);
    EXPECT_EQ(eligibility.released_at, at_time(100));
}

TEST(QueueCoreTest, ReleaseLedgerPrunesOnlyPastWindowAndClockLeeway) {
    QueueCore core(10, 10s, 30min, 100, 5min);
    static_cast<void>(core.issue("jti-a", at_time(0)));
    ASSERT_EQ(
        core.release_batch(BudgetAggregate{10, 10}, at_time(100)), 1U);

    EXPECT_EQ(
        core.release_eligibility(1, at_time(460)).status,
        QueueReleaseStatus::Eligible);
    static_cast<void>(
        core.release_batch(BudgetAggregate{0, 0}, at_time(461)));
    EXPECT_EQ(
        core.release_eligibility(1, at_time(461)).status,
        QueueReleaseStatus::Expired);
    const auto snapshot = core.snapshot(at_time(461));
    EXPECT_EQ(snapshot.release_batches_pruned_through, 1U);
    EXPECT_TRUE(snapshot.release_batches.empty());
}

TEST(QueueCoreTest, ReleaseBatchBoundedByIssuedAhead) {
    QueueCore core(100);
    for (int index = 0; index < 5; ++index) {
        static_cast<void>(core.issue("jti-" + std::to_string(index), at_time(0)));
    }
    const BudgetAggregate budgets{100, 100};
    EXPECT_EQ(core.release_batch(budgets, at_time(0)), 5U);
    EXPECT_EQ(core.release_batch(budgets, at_time(2)), 0U);
}

TEST(QueueCoreTest, ReleaseBatchFailsClosedWithoutBudgets) {
    QueueCore core(100);
    static_cast<void>(core.issue("jti-a", at_time(0)));
    EXPECT_EQ(core.release_batch(BudgetAggregate{0, 100}, at_time(0)), 0U);
    EXPECT_EQ(core.release_batch(BudgetAggregate{100, 0}, at_time(0)), 0U);
    EXPECT_EQ(core.released_number(), 0U);
}

TEST(QueueCoreTest, AggregationUsesPerInstanceMinimum) {
    const std::vector<GatewayBudget> gateways{
        {100, 30}, {0, 100}, {50, 50}};
    const std::vector<RealmBudget> realms{{40}, {10}};
    const auto aggregate = aggregate_budgets(gateways, realms);
    EXPECT_EQ(aggregate.gateway_admission, 80U);
    EXPECT_EQ(aggregate.realm_connections, 50U);
}

TEST(QueueCoreTest, AdmitRateMeasuredOverFixedWindow) {
    QueueCore core(3000);
    for (int index = 0; index < 6000; ++index) {
        static_cast<void>(core.issue("jti-" + std::to_string(index), at_time(0)));
    }
    static_cast<void>(core.release_batch(BudgetAggregate{3000, 3000}, at_time(0)));
    static_cast<void>(core.release_batch(BudgetAggregate{3000, 3000}, at_time(2)));
    EXPECT_EQ(core.admit_rate(at_time(2)), 600U);
    // 只剩 t=2s 的 3000 在窗内(300/s),窗滑过后归零。
    EXPECT_EQ(core.admit_rate(at_time(11)), 300U);
    EXPECT_EQ(core.admit_rate(at_time(13)), 0U);
}

TEST(QueueCoreTest, SnapshotRoundTripsThroughRestore) {
    QueueCore source(100);
    for (int index = 0; index < 3; ++index) {
        static_cast<void>(source.issue("jti-" + std::to_string(index), at_time(0)));
    }
    static_cast<void>(source.release_batch(BudgetAggregate{100, 100}, at_time(0)));
    const auto snapshot = source.snapshot(at_time(0));
    EXPECT_EQ(snapshot.released_number, 3U);
    EXPECT_EQ(snapshot.next_number, 4U);
    ASSERT_EQ(snapshot.release_batches.size(), 1U);
    EXPECT_EQ(snapshot.release_batches.front().first_number, 1U);
    EXPECT_EQ(snapshot.release_batches.front().last_number, 3U);
    EXPECT_EQ(snapshot.release_batches.front().released_at, at_time(0));

    QueueCore restored(100);
    restored.restore(snapshot, at_time(1));
    EXPECT_EQ(restored.released_number(), 3U);
    EXPECT_EQ(restored.next_number(), 4U);
    EXPECT_EQ(restored.admit_rate(at_time(1)), 0U);
    EXPECT_EQ(
        restored.issue("jti-new", at_time(1)), (QueueCore::Issued{4, true}));
    // 已发未放行 = 1,放行量只到 1。
    EXPECT_EQ(
        restored.release_batch(BudgetAggregate{100, 100}, at_time(2)), 1U);
    EXPECT_EQ(restored.released_number(), 4U);
}

TEST(QueueCoreTest, RestartPreservesWindowAndCannotRenewIt) {
    QueueCore source(100, 10s, 30min, 100, 5min);
    static_cast<void>(source.issue("jti-a", at_time(0)));
    static_cast<void>(
        source.release_batch(BudgetAggregate{100, 100}, at_time(100)));

    QueueCore restored(100, 10s, 30min, 100, 5min);
    restored.restore(source.snapshot(at_time(120)), at_time(200));
    const auto active = restored.release_eligibility(1, at_time(200));
    EXPECT_EQ(active.status, QueueReleaseStatus::Eligible);
    EXPECT_EQ(active.released_at, at_time(100));
    EXPECT_EQ(
        restored.release_eligibility(1, at_time(461)).status,
        QueueReleaseStatus::Expired);
}

TEST(QueueCoreTest, RestoreRejectsInconsistentSnapshot) {
    QueueCore core(100);
    EXPECT_THROW(
        core.restore(QueueSnapshot{5, 5, 0}), std::invalid_argument);
    EXPECT_THROW(
        core.restore(QueueSnapshot{6, 5, 0}), std::invalid_argument);

    QueueSnapshot gap{
        .released_number = 5,
        .next_number = 6,
        .release_batches_pruned_through = 1,
        .release_batches = {
            {.first_number = 3,
             .last_number = 5,
             .released_at = at_time(10)}},
    };
    EXPECT_THROW(core.restore(gap, at_time(20)), std::invalid_argument);

    QueueSnapshot future{
        .released_number = 1,
        .next_number = 2,
        .release_batches = {
            {.first_number = 1,
             .last_number = 1,
             .released_at = at_time(100)}},
    };
    EXPECT_THROW(core.restore(future, at_time(0)), std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::queue
