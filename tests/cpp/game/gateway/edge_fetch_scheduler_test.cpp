#include "realmmesh/game/gateway/edge_fetch_scheduler.hpp"

#include "realmmesh/game/gateway/edge_fetch.hpp"
#include "realmmesh/game/gateway/edge_session_pipeline.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <utility>
#include <vector>

namespace realm::game::gateway {
namespace {

/// 脚本化拉取源:按序吐出预置结果;脚本耗尽即失败(防止测试静默多打)。
class ScriptedFetchSource final : public EdgeFetchSource {
public:
    explicit ScriptedFetchSource(std::deque<EdgeFetchOutcome> script)
        : script_(std::move(script)) {}

    [[nodiscard]] EdgeFetchOutcome fetch(std::uint64_t account_id) override {
        ++calls_;
        EXPECT_FALSE(script_.empty())
            << "unexpected fetch attempt for account " << account_id;
        if (script_.empty()) {
            return {false, std::chrono::milliseconds{0}};
        }
        const auto outcome = script_.front();
        script_.pop_front();
        return outcome;
    }

    [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

private:
    std::deque<EdgeFetchOutcome> script_;
    std::size_t calls_{0};
};

/// 与墙钟脱钩的固定基准时刻;退避断言全部相对它展开。
constexpr auto t0 = std::chrono::steady_clock::time_point{
    std::chrono::milliseconds{1'000'000}};

TEST(EdgeFetchSchedulerTest, SuccessCompletesAndReleasesFetchSlot) {
    EdgeSessionPipeline pipeline{4, 2};
    ScriptedFetchSource source({{true, std::chrono::milliseconds{50}}});
    EdgeFetchScheduler scheduler(std::chrono::milliseconds{2'000}, 3, source);

    const EdgeSessionId session{1};
    pipeline.on_session_opened(session);
    ASSERT_EQ(
        pipeline.try_enter_fetching(session), EnterFetchingResult::Entered);
    scheduler.register_session(session, 42, t0);

    // 在途未到点:不结算、不重复发起。
    EXPECT_TRUE(scheduler.tick(t0).empty());
    EXPECT_TRUE(scheduler.tick(t0 + std::chrono::milliseconds{49}).empty());
    ASSERT_EQ(source.calls(), 1U);

    auto events = scheduler.tick(t0 + std::chrono::milliseconds{50});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].session_id, session);
    EXPECT_EQ(events[0].kind, EdgeFetchEventKind::Succeeded);

    // 调用方应用事件:迁 handed-off,拉取槽即还。
    ASSERT_TRUE(pipeline.mark_handed_off(session));
    EXPECT_EQ(pipeline.stage(session), EdgeSessionStage::HandedOff);
    EXPECT_EQ(pipeline.fetch_used(), 0U);
    EXPECT_EQ(scheduler.active(), 0U);
    EXPECT_EQ(source.calls(), 1U);
}

TEST(EdgeFetchSchedulerTest, ExhaustedAfterRetryMaxWithExponentialBackoff) {
    constexpr auto fail_now = EdgeFetchOutcome{
        false, std::chrono::milliseconds{0}};
    EdgeSessionPipeline pipeline{4, 2};
    ScriptedFetchSource source({fail_now, fail_now, fail_now, fail_now});
    EdgeFetchScheduler scheduler(std::chrono::milliseconds{2'000}, 3, source);

    const EdgeSessionId session{7};
    pipeline.on_session_opened(session);
    ASSERT_EQ(
        pipeline.try_enter_fetching(session), EnterFetchingResult::Entered);
    EXPECT_EQ(pipeline.fetch_used(), 1U);
    scheduler.register_session(session, 42, t0);

    // 首发(0 耗时)同帧结算:退避 2s;1999ms 处不重试。
    EXPECT_TRUE(scheduler.tick(t0).empty());
    EXPECT_TRUE(scheduler.tick(t0 + std::chrono::milliseconds{1'999}).empty());
    EXPECT_EQ(source.calls(), 1U);

    // #2(+2s,退避 4s)、#3(+6s,退避 8s)。
    EXPECT_TRUE(scheduler.tick(t0 + std::chrono::milliseconds{2'000}).empty());
    EXPECT_TRUE(scheduler.tick(t0 + std::chrono::milliseconds{5'999}).empty());
    EXPECT_TRUE(scheduler.tick(t0 + std::chrono::milliseconds{6'000}).empty());
    EXPECT_TRUE(scheduler.tick(t0 + std::chrono::milliseconds{13'999}).empty());
    EXPECT_EQ(source.calls(), 3U);

    // #4(+14s)失败即耗尽:上报 Exhausted,条目出表。
    auto events = scheduler.tick(t0 + std::chrono::milliseconds{14'000});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].session_id, session);
    EXPECT_EQ(events[0].kind, EdgeFetchEventKind::Exhausted);
    EXPECT_EQ(source.calls(), 4U);
    EXPECT_EQ(scheduler.active(), 0U);

    // 调用方按 #43 关闭路径终结会话:conn 与 fetch 双预算归还。
    const auto closed = pipeline.on_session_closed(session);
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->stage, EdgeSessionStage::Fetching);
    EXPECT_EQ(pipeline.conn_used(), 0U);
    EXPECT_EQ(pipeline.fetch_used(), 0U);
}

TEST(EdgeFetchSchedulerTest, CancelDropsInFlightAttempt) {
    ScriptedFetchSource source({{false, std::chrono::seconds{10}}});
    EdgeFetchScheduler scheduler(std::chrono::milliseconds{2'000}, 3, source);

    const EdgeSessionId session{3};
    scheduler.register_session(session, 42, t0);
    EXPECT_TRUE(scheduler.tick(t0).empty());
    ASSERT_EQ(source.calls(), 1U);

    // 对端中途断开:在途尝试随之作废,到点不再结算。
    scheduler.cancel(session);
    EXPECT_TRUE(scheduler.tick(t0 + std::chrono::seconds{10}).empty());
    EXPECT_EQ(source.calls(), 1U);
    EXPECT_EQ(scheduler.active(), 0U);
}

TEST(EdgeFetchSchedulerTest, DefaultStubSucceedsWithConfiguredLatency) {
    DelayedFetchSource source(std::chrono::milliseconds{10});
    EdgeFetchScheduler scheduler(std::chrono::milliseconds{2'000}, 3, source);

    const EdgeSessionId session{9};
    scheduler.register_session(session, 42, t0);
    EXPECT_TRUE(scheduler.tick(t0).empty());
    auto events = scheduler.tick(t0 + std::chrono::milliseconds{10});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, EdgeFetchEventKind::Succeeded);
    EXPECT_EQ(source.fetch(std::uint64_t{0}).ok, true);
}

TEST(EdgeFetchSchedulerTest, RetryMaxOneExhaustsOnSecondAttempt) {
    constexpr auto fail_now = EdgeFetchOutcome{
        false, std::chrono::milliseconds{0}};
    ScriptedFetchSource source({fail_now, fail_now});
    EdgeFetchScheduler scheduler(std::chrono::milliseconds{500}, 1, source);

    const EdgeSessionId session{5};
    scheduler.register_session(session, 42, t0);
    EXPECT_TRUE(scheduler.tick(t0).empty());
    auto events = scheduler.tick(t0 + std::chrono::milliseconds{500});
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].kind, EdgeFetchEventKind::Exhausted);
    EXPECT_EQ(source.calls(), 2U);
    EXPECT_EQ(scheduler.active(), 0U);
}

}  // namespace
}  // namespace realm::game::gateway
