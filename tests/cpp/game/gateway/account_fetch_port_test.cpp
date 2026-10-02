#include "realmmesh/game/gateway/account_fetch_port.hpp"

#include "realmmesh/game/common/player_data_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>

namespace realm::game::gateway {
namespace {

using namespace std::chrono_literals;

[[nodiscard]] std::optional<AccountFetchCompletion> wait_for_completion(
    AccountFetchPort& port) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline) {
        auto completions =
            port.drain_completions(std::chrono::steady_clock::now(), 1);
        if (!completions.empty()) return std::move(completions.front());
        std::this_thread::sleep_for(1ms);
    }
    return std::nullopt;
}

TEST(DelayedAccountFetchPortTest, CompletesWithoutBlockingTheSubmitter) {
    DelayedAccountFetchPort port(100ms, 2);
    const auto now = std::chrono::steady_clock::time_point{};

    EXPECT_EQ(
        port.submit({AccountFetchAttemptId{11}, EdgeSessionId{3}, 42}, now),
        AccountFetchSubmitResult::Submitted);
    EXPECT_TRUE(port.drain_completions(now + 99ms, 8).empty());

    const auto completions = port.drain_completions(now + 100ms, 8);
    ASSERT_EQ(completions.size(), 1U);
    EXPECT_EQ(completions[0].attempt_id, AccountFetchAttemptId{11});
    EXPECT_TRUE(completions[0].ok);
    EXPECT_EQ(completions[0].duration, 100ms);
}

TEST(DelayedAccountFetchPortTest, SeparatesBackpressureFromFailureCompletion) {
    DelayedAccountFetchPort port(10ms, 1);
    const auto now = std::chrono::steady_clock::time_point{};

    EXPECT_EQ(
        port.submit({AccountFetchAttemptId{1}, EdgeSessionId{1}, 10}, now),
        AccountFetchSubmitResult::Submitted);
    EXPECT_EQ(
        port.submit({AccountFetchAttemptId{2}, EdgeSessionId{2}, 20}, now),
        AccountFetchSubmitResult::Full);
    port.cancel(AccountFetchAttemptId{1});
    EXPECT_EQ(
        port.submit({AccountFetchAttemptId{2}, EdgeSessionId{2}, 20}, now),
        AccountFetchSubmitResult::Submitted);
}

TEST(ScriptedAccountFetchPortTest, PreservesAttemptIdentityForLateDuplicates) {
    ScriptedAccountFetchPort port;
    const auto now = std::chrono::steady_clock::time_point{};
    ASSERT_EQ(
        port.submit({AccountFetchAttemptId{8}, EdgeSessionId{1}, 10}, now),
        AccountFetchSubmitResult::Submitted);
    ASSERT_EQ(
        port.submit({AccountFetchAttemptId{9}, EdgeSessionId{1}, 10}, now),
        AccountFetchSubmitResult::Submitted);
    port.cancel(AccountFetchAttemptId{8});

    port.push_completion({AccountFetchAttemptId{8}, true, 1ms});
    port.push_completion({AccountFetchAttemptId{8}, true, 2ms});
    port.push_completion({AccountFetchAttemptId{9}, false, 3ms});
    const auto completions = port.drain_completions(now, 8);

    ASSERT_EQ(completions.size(), 3U);
    EXPECT_EQ(completions[0].attempt_id, AccountFetchAttemptId{8});
    EXPECT_EQ(completions[1].attempt_id, AccountFetchAttemptId{8});
    EXPECT_EQ(completions[2].attempt_id, AccountFetchAttemptId{9});
    EXPECT_TRUE(port.was_cancelled(AccountFetchAttemptId{8}));
}

TEST(ScriptedAccountFetchPortTest, ScriptsFullAndStoppedSubmissions) {
    ScriptedAccountFetchPort port;
    port.script_submit_results(
        {AccountFetchSubmitResult::Full, AccountFetchSubmitResult::Stopped});
    const auto request =
        AccountFetchRequest{AccountFetchAttemptId{1}, EdgeSessionId{2}, 3};
    const auto now = std::chrono::steady_clock::time_point{};

    EXPECT_EQ(port.submit(request, now), AccountFetchSubmitResult::Full);
    EXPECT_EQ(port.submit(request, now), AccountFetchSubmitResult::Stopped);
    EXPECT_EQ(port.submit(request, now), AccountFetchSubmitResult::Stopped);
    EXPECT_TRUE(port.submitted_requests().empty());
}

TEST(ScriptedAccountFetchPortTest, StopIsTerminalForTheAdapterInstance) {
    ScriptedAccountFetchPort port;
    port.stop();
    const auto request =
        AccountFetchRequest{AccountFetchAttemptId{7}, EdgeSessionId{2}, 3};

    EXPECT_EQ(
        port.submit(request, std::chrono::steady_clock::time_point{}),
        AccountFetchSubmitResult::Stopped);
    EXPECT_TRUE(port.submitted_requests().empty());
}

/// 内存 PlayerDataReader:端口的契约(工作线程查询、每次重新确认准入、
/// 存储故障映射为 Unavailable)与具体数据库无关；真实 MongoDB 的读写语义
/// 由 player_data_store_test 覆盖。
class FakePlayerDataReader final : public common::PlayerDataReader {
public:
    struct State final {
        std::mutex mutex;
        std::condition_variable released_condition;
        bool released{true};
        bool eligible{true};
        bool unavailable{false};
        std::atomic<int> queries{0};
    };

    explicit FakePlayerDataReader(std::shared_ptr<State> state)
        : state_(std::move(state)) {}

    [[nodiscard]] std::optional<common::AccountLoginFacts> login_facts(
        std::uint64_t account_id) const override {
        ++state_->queries;
        std::unique_lock lock(state_->mutex);
        state_->released_condition.wait(lock, [this] { return state_->released; });
        if (state_->unavailable) {
            throw common::PlayerDataError("player data unavailable");
        }
        if (!state_->eligible) return std::nullopt;
        return common::AccountLoginFacts{.account_id = account_id};
    }

private:
    std::shared_ptr<State> state_;
};

TEST(PlayerDataAccountFetchPortTest, LoadsEligibilityWithoutBlockingSubmit) {
    const auto state = std::make_shared<FakePlayerDataReader::State>();
    state->released = false;
    PlayerDataAccountFetchPort port(
        std::make_unique<FakePlayerDataReader>(state), 1, 1);
    const auto now = std::chrono::steady_clock::now();
    // 查询被挡在工作线程里，submit 仍立即返回。
    EXPECT_EQ(
        port.submit({AccountFetchAttemptId{1}, EdgeSessionId{1}, 42}, now),
        AccountFetchSubmitResult::Submitted);
    EXPECT_EQ(
        port.submit({AccountFetchAttemptId{2}, EdgeSessionId{2}, 42}, now),
        AccountFetchSubmitResult::Full);
    {
        const std::scoped_lock lock(state->mutex);
        state->released = true;
    }
    state->released_condition.notify_all();

    const auto completion = wait_for_completion(port);
    ASSERT_TRUE(completion.has_value());
    EXPECT_TRUE(completion->ok);
    EXPECT_EQ(completion->status, AccountFetchStatus::Succeeded);
}

TEST(PlayerDataAccountFetchPortTest, SlowQueriesDoNotSerializeAcrossWorkers) {
    const auto state = std::make_shared<FakePlayerDataReader::State>();
    state->released = false;
    PlayerDataAccountFetchPort port(
        std::make_unique<FakePlayerDataReader>(state), 4, 2);
    const auto now = std::chrono::steady_clock::now();
    ASSERT_EQ(
        port.submit({AccountFetchAttemptId{1}, EdgeSessionId{1}, 41}, now),
        AccountFetchSubmitResult::Submitted);
    ASSERT_EQ(
        port.submit({AccountFetchAttemptId{2}, EdgeSessionId{2}, 42}, now),
        AccountFetchSubmitResult::Submitted);

    // 第一个查询卡住时,第二个工作线程仍接走第二个查询。
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (state->queries.load() < 2 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(state->queries.load(), 2);

    {
        const std::scoped_lock lock(state->mutex);
        state->released = true;
    }
    state->released_condition.notify_all();
    ASSERT_TRUE(wait_for_completion(port).has_value());
    ASSERT_TRUE(wait_for_completion(port).has_value());
}

TEST(PlayerDataAccountFetchPortTest, RechecksAccountAccessOnEveryAttempt) {
    const auto state = std::make_shared<FakePlayerDataReader::State>();
    PlayerDataAccountFetchPort port(
        std::make_unique<FakePlayerDataReader>(state), 2, 1);
    ASSERT_EQ(
        port.submit(
            {AccountFetchAttemptId{1}, EdgeSessionId{1}, 42},
            std::chrono::steady_clock::now()),
        AccountFetchSubmitResult::Submitted);
    const auto first = wait_for_completion(port);
    ASSERT_TRUE(first.has_value());
    EXPECT_EQ(first->status, AccountFetchStatus::Succeeded);

    {
        const std::scoped_lock lock(state->mutex);
        state->eligible = false;
    }
    ASSERT_EQ(
        port.submit(
            {AccountFetchAttemptId{2}, EdgeSessionId{1}, 42},
            std::chrono::steady_clock::now()),
        AccountFetchSubmitResult::Submitted);
    const auto second = wait_for_completion(port);
    ASSERT_TRUE(second.has_value());
    EXPECT_FALSE(second->ok);
    EXPECT_EQ(second->status, AccountFetchStatus::NotEligible);
    EXPECT_EQ(state->queries.load(), 2);
}

TEST(PlayerDataAccountFetchPortTest, StoreFailureCompletesAsUnavailable) {
    const auto state = std::make_shared<FakePlayerDataReader::State>();
    state->unavailable = true;
    PlayerDataAccountFetchPort port(
        std::make_unique<FakePlayerDataReader>(state), 1, 1);
    ASSERT_EQ(
        port.submit(
            {AccountFetchAttemptId{1}, EdgeSessionId{1}, 42},
            std::chrono::steady_clock::now()),
        AccountFetchSubmitResult::Submitted);

    const auto completion = wait_for_completion(port);
    ASSERT_TRUE(completion.has_value());
    EXPECT_FALSE(completion->ok);
    EXPECT_EQ(completion->status, AccountFetchStatus::Unavailable);
}

TEST(PlayerDataAccountFetchPortTest, RejectsMissingReaderZeroCapacityAndZeroWorkers) {
    const auto state = std::make_shared<FakePlayerDataReader::State>();
    EXPECT_THROW(
        PlayerDataAccountFetchPort(nullptr, 1, 1), std::invalid_argument);
    EXPECT_THROW(
        PlayerDataAccountFetchPort(
            std::make_unique<FakePlayerDataReader>(state), 0, 1),
        std::invalid_argument);
    EXPECT_THROW(
        PlayerDataAccountFetchPort(
            std::make_unique<FakePlayerDataReader>(state), 1, 0),
        std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::gateway
