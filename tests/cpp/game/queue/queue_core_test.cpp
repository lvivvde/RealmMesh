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

    QueueCore restored(100);
    restored.restore(snapshot);
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

TEST(QueueCoreTest, RestoreRejectsInconsistentSnapshot) {
    QueueCore core(100);
    EXPECT_THROW(
        core.restore(QueueSnapshot{5, 5, 0}), std::invalid_argument);
    EXPECT_THROW(
        core.restore(QueueSnapshot{6, 5, 0}), std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::queue
