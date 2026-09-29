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

void restore_issued(
    QueueCore& core,
    std::uint64_t issued_count,
    TimePoint now = at_time(0)) {
    auto snapshot = core.snapshot(now);
    snapshot.next_number = issued_count + 1U;
    core.restore(snapshot, now);
}

TEST(QueueCoreTest, ReleaseBatchHonorsStepAndBudgets) {
    QueueCore core(100);
    restore_issued(core, 250);
    const BudgetAggregate budgets{500, 500};
    EXPECT_EQ(core.release_batch(budgets, at_time(0)), 100U);
    EXPECT_EQ(core.released_number(), 100U);
    EXPECT_EQ(core.release_batch(budgets, at_time(2)), 100U);
    EXPECT_EQ(core.release_batch(budgets, at_time(4)), 50U);
    EXPECT_EQ(core.release_batch(budgets, at_time(6)), 0U);
}

TEST(QueueCoreTest, ReleaseLedgerMapsEveryBatchBoundaryToStableTime) {
    QueueCore core(3);
    restore_issued(core, 6);
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
    restore_issued(core, 1);
    ASSERT_EQ(
        core.release_batch(
            BudgetAggregate{1, 1}, at_time(100) + 750ms),
        1U);
    const auto eligibility = core.release_eligibility(1, at_time(101));
    EXPECT_EQ(eligibility.status, QueueReleaseStatus::Eligible);
    EXPECT_EQ(eligibility.released_at, at_time(100));
}

TEST(QueueCoreTest, ReleaseLedgerPrunesOnlyPastWindowAndClockLeeway) {
    QueueCore core(10, 10s, 5min);
    restore_issued(core, 1);
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
    restore_issued(core, 5);
    const BudgetAggregate budgets{100, 100};
    EXPECT_EQ(core.release_batch(budgets, at_time(0)), 5U);
    EXPECT_EQ(core.release_batch(budgets, at_time(2)), 0U);
}

TEST(QueueCoreTest, ReleaseBatchFailsClosedWithoutBudgets) {
    QueueCore core(100);
    restore_issued(core, 1);
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
    restore_issued(core, 6000);
    static_cast<void>(core.release_batch(BudgetAggregate{3000, 3000}, at_time(0)));
    static_cast<void>(core.release_batch(BudgetAggregate{3000, 3000}, at_time(2)));
    EXPECT_EQ(core.admit_rate(at_time(2)), 600U);
    // 只剩 t=2s 的 3000 在窗内(300/s),窗滑过后归零。
    EXPECT_EQ(core.admit_rate(at_time(11)), 300U);
    EXPECT_EQ(core.admit_rate(at_time(13)), 0U);
}

TEST(QueueCoreTest, SnapshotRoundTripsThroughRestore) {
    QueueCore source(100);
    restore_issued(source, 3);
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
    // 发号事务提交后的新快照由 QueueTicketing 恢复进域核心。
    restore_issued(restored, 4, at_time(1));
    // 已发未放行 = 1,放行量只到 1。
    EXPECT_EQ(
        restored.release_batch(BudgetAggregate{100, 100}, at_time(2)), 1U);
    EXPECT_EQ(restored.released_number(), 4U);
}

TEST(QueueCoreTest, RestartPreservesWindowAndCannotRenewIt) {
    QueueCore source(100, 10s, 5min);
    restore_issued(source, 1);
    static_cast<void>(
        source.release_batch(BudgetAggregate{100, 100}, at_time(100)));

    QueueCore restored(100, 10s, 5min);
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
