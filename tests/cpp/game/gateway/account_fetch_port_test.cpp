#include "realmmesh/game/gateway/account_fetch_port.hpp"

#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <optional>
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

TEST(SqliteAccountFetchPortTest, LoadsSelectedCharacterWithoutBlockingSubmit) {
    test_support::TemporaryDirectory directory{"sqlite-fetch-port-test-"};
    const auto database = directory.path() / "player-data.sqlite";
    {
        common::SqlitePlayerDataStore store(database);
        store.provision_account(common::AccountProvisioning{
            .account_id = 42,
            .account_name = "player",
            .credential = "secret",
            .whitelisted = true,
        });
        store.provision_character(common::CharacterRecord{
            .character_id = 7001,
            .account_id = 42,
            .realm_id = 1,
            .name = "Ranger",
            .revision = 4,
        });
        store.select_character(42, 7001);
    }

    SqliteAccountFetchPort port(
        database, 1, common::SqlitePlayerDataOptions{.busy_timeout = 250ms});
    const auto now = std::chrono::steady_clock::now();
    EXPECT_EQ(
        port.submit({AccountFetchAttemptId{1}, EdgeSessionId{1}, 42}, now),
        AccountFetchSubmitResult::Submitted);
    EXPECT_EQ(
        port.submit({AccountFetchAttemptId{2}, EdgeSessionId{2}, 42}, now),
        AccountFetchSubmitResult::Full);

    const auto completion = wait_for_completion(port);
    ASSERT_TRUE(completion.has_value());
    EXPECT_TRUE(completion->ok);
    EXPECT_EQ(completion->status, AccountFetchStatus::Succeeded);
    EXPECT_EQ(completion->character_id, 7001U);
    EXPECT_EQ(completion->realm_id, 1U);
    EXPECT_EQ(completion->character_revision, 4U);
}

TEST(SqliteAccountFetchPortTest, RechecksAccountAccessOnEveryAttempt) {
    test_support::TemporaryDirectory directory{"sqlite-fetch-port-test-"};
    const auto database = directory.path() / "player-data.sqlite";
    common::SqlitePlayerDataStore writer(database);
    writer.provision_account(common::AccountProvisioning{
        .account_id = 42,
        .account_name = "player",
        .credential = "secret",
        .whitelisted = true,
    });
    writer.provision_character(common::CharacterRecord{
        .character_id = 7001,
        .account_id = 42,
        .realm_id = 1,
        .name = "Ranger",
    });
    writer.select_character(42, 7001);

    SqliteAccountFetchPort port(
        database, 2, common::SqlitePlayerDataOptions{.busy_timeout = 250ms});
    writer.set_account_access(42, true, true);
    ASSERT_EQ(
        port.submit(
            {AccountFetchAttemptId{1}, EdgeSessionId{1}, 42},
            std::chrono::steady_clock::now()),
        AccountFetchSubmitResult::Submitted);

    const auto completion = wait_for_completion(port);
    ASSERT_TRUE(completion.has_value());
    EXPECT_FALSE(completion->ok);
    EXPECT_EQ(completion->status, AccountFetchStatus::NotEligible);
}

}  // namespace
}  // namespace realm::game::gateway
