#include "realmmesh/game/realm/realm_sessions.hpp"

#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/realm_protocol.hpp"
#include "realmmesh/game/realm/training_rule.hpp"
#include "realmmesh/test_support/in_memory_character_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using namespace std::chrono_literals;
using realm::game::common::CharacterRoster;
using realm::game::common::CreateCharacterOutcome;
using realm::game::common::CreateCharacterResult;
using realm::game::common::PlayerDataError;
using realm::game::common::RealmCharacter;
using realm::game::common::TrainingWrite;
using realm::game::common::TrainingWriteResult;
using realm::game::gateway::EdgeSessionId;
using realm::game::realm::RealmConfig;
using realm::game::realm::RealmSessionPhase;
using realm::game::realm::RealmSessions;
using realm::game::realm::TrainingRule;
namespace common = realm::game::common;

class RecordingOutbox final : public realm::game::realm::RealmOutbox {
public:
    struct Sent {
        EdgeSessionId session;
        std::vector<std::byte> payload;
    };

    void send(EdgeSessionId session, std::vector<std::byte> payload) override {
        sent.push_back({session, std::move(payload)});
    }
    void close(EdgeSessionId session) override { closed.insert(session.value); }

    std::vector<Sent> sent;
    std::set<std::uint64_t> closed;
};

const std::filesystem::path shipped_rule =
    std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs" / "services" /
    "realm" / "training.lua";

constexpr EdgeSessionId first{1};
constexpr EdgeSessionId second{2};
constexpr std::uint64_t account = 42;

class RealmSessionsTest : public ::testing::Test {
protected:
    RealmSessionsTest() : rule_(shipped_rule) {}

    [[nodiscard]] RealmSessions& sessions(RealmConfig config = {}) {
        return sessions_with(rule_, config);
    }

    [[nodiscard]] RealmSessions& sessions_with(
        TrainingRule& rule, RealmConfig config = {}) {
        config.training_rule_file = shipped_rule;
        sessions_.emplace(store_, rule, outbox_, config);
        return *sessions_;
    }

    template <typename Message>
    void request(EdgeSessionId session, const Message& message,
                 std::uint64_t request_id) {
        ASSERT_TRUE(sessions_->handle(session, common::encode(message, request_id)));
    }

    /// 推进帧尾,直到 session 收到第 count 条回包。
    [[nodiscard]] const std::vector<std::byte>& reply(
        EdgeSessionId session, std::size_t count = 1) {
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (std::chrono::steady_clock::now() < deadline) {
            sessions_->complete(64);
            const auto replies = for_session(session);
            if (replies.size() >= count) return *replies[count - 1];
            std::this_thread::sleep_for(1ms);
        }
        ADD_FAILURE() << "no reply " << count << " for session "
                      << session.value;
        static const std::vector<std::byte> empty;
        return empty;
    }

    [[nodiscard]] std::vector<const std::vector<std::byte>*> for_session(
        EdgeSessionId session) const {
        std::vector<const std::vector<std::byte>*> replies;
        for (const auto& sent : outbox_.sent) {
            if (sent.session == session) replies.push_back(&sent.payload);
        }
        return replies;
    }

    [[nodiscard]] static int error_code(const std::vector<std::byte>& payload) {
        const auto error = common::decode_edge_error(payload);
        return error.has_value() ? static_cast<int>(error->code()) : -1;
    }

    /// 入场、创建一个角色并选中,回包计数推进到 2。
    [[nodiscard]] std::uint64_t enter_game(EdgeSessionId session) {
        sessions_->enter(session, account);
        common::CreateCharacter create;
        create.set_name("Ranger");
        request(session, create, 1);
        const auto created =
            common::decode_character_created(reply(session, 1));
        EXPECT_TRUE(created.has_value());
        const auto id = created->character().character_id();
        common::SelectCharacter select;
        select.set_character_id(id);
        request(session, select, 2);
        EXPECT_TRUE(common::decode_character_selected(reply(session, 2)));
        return id;
    }

    realm::test_support::InMemoryCharacterStore store_;
    TrainingRule rule_;
    RecordingOutbox outbox_;
    std::optional<RealmSessions> sessions_;
};

TEST_F(RealmSessionsTest, NewAccountListsEmptyRosterInSelection) {
    auto& realm = sessions();
    realm.enter(first, account);
    EXPECT_EQ(realm.phase(first), RealmSessionPhase::Selecting);
    request(first, common::ListCharacters{}, 7);
    const auto& payload = reply(first);
    EXPECT_EQ(common::realm_request_id(payload), 7U);
    const auto list = common::decode_character_list(payload);
    ASSERT_TRUE(list.has_value());
    EXPECT_EQ(list->characters_size(), 0);
    EXPECT_EQ(list->last_selected_character_id(), 0U);
}

TEST_F(RealmSessionsTest, ListCarriesLevelAndLastSelected) {
    store_.add(account, {.character_id = 5, .name = "Old", .exp = 250});
    auto& realm = sessions();
    realm.enter(first, account);
    common::SelectCharacter select;
    select.set_character_id(5);
    request(first, select, 1);
    static_cast<void>(reply(first, 1));
    sessions_->closed(first);

    realm.enter(second, account);
    request(second, common::ListCharacters{}, 2);
    const auto list = common::decode_character_list(reply(second));
    ASSERT_TRUE(list.has_value());
    ASSERT_EQ(list->characters_size(), 1);
    EXPECT_EQ(list->characters(0).name(), "Old");
    EXPECT_EQ(list->characters(0).exp(), 250U);
    EXPECT_EQ(list->characters(0).level(), 3U);
    EXPECT_EQ(list->last_selected_character_id(), 5U);
}

TEST_F(RealmSessionsTest, InvalidNameIsRejectedWithoutTouchingStore) {
    auto& realm = sessions();
    realm.enter(first, account);
    common::CreateCharacter create;
    create.set_name(" padded");
    request(first, create, 3);
    EXPECT_EQ(error_code(reply(first)), common::edge_error_invalid_character_name);
    EXPECT_EQ(store_.calls(), 0);
    EXPECT_EQ(realm.phase(first), RealmSessionPhase::Selecting);
}

TEST_F(RealmSessionsTest, CreateMapsTakenNameAndLimit) {
    store_.add(7, {.character_id = 1, .name = "Taken"});
    store_.add(account, {.character_id = 2, .name = "A"});
    store_.add(account, {.character_id = 3, .name = "B"});
    auto& realm = sessions();
    realm.enter(first, account);
    common::CreateCharacter create;
    create.set_name("Taken");
    request(first, create, 1);
    EXPECT_EQ(error_code(reply(first, 1)), common::edge_error_character_name_taken);

    create.set_name("C");
    request(first, create, 2);
    const auto created = common::decode_character_created(reply(first, 2));
    ASSERT_TRUE(created.has_value());
    EXPECT_EQ(created->character().name(), "C");
    EXPECT_EQ(created->character().exp(), 0U);
    EXPECT_EQ(created->character().level(), 1U);

    create.set_name("D");
    request(first, create, 3);
    EXPECT_EQ(
        error_code(reply(first, 3)), common::edge_error_character_limit_reached);
}

TEST_F(RealmSessionsTest, SelectingForeignCharacterIsRejected) {
    store_.add(7, {.character_id = 11, .name = "Foreign"});
    auto& realm = sessions();
    realm.enter(first, account);
    common::SelectCharacter select;
    select.set_character_id(11);
    request(first, select, 4);
    EXPECT_EQ(error_code(reply(first)), common::edge_error_character_not_owned);
    EXPECT_EQ(realm.phase(first), RealmSessionPhase::Selecting);
    EXPECT_EQ(store_.last_selected(account), 0U);
}

TEST_F(RealmSessionsTest, PhaseMismatchKeepsSessionOpen) {
    auto& realm = sessions();
    realm.enter(first, account);
    common::Train train;
    train.set_seq(1);
    request(first, train, 1);
    EXPECT_EQ(error_code(reply(first, 1)), common::edge_error_phase_mismatch);

    common::CreateCharacter create;
    create.set_name("Ranger");
    request(first, create, 2);
    const auto created = common::decode_character_created(reply(first, 2));
    ASSERT_TRUE(created.has_value());
    common::SelectCharacter select;
    select.set_character_id(created->character().character_id());
    request(first, select, 3);
    const auto selected = common::decode_character_selected(reply(first, 3));
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->training_seq(), 0U);
    EXPECT_EQ(realm.phase(first), RealmSessionPhase::InGame);
    EXPECT_EQ(store_.last_selected(account), created->character().character_id());

    request(first, common::ListCharacters{}, 4);
    EXPECT_EQ(error_code(reply(first, 4)), common::edge_error_phase_mismatch);
    request(first, select, 5);
    EXPECT_EQ(error_code(reply(first, 5)), common::edge_error_phase_mismatch);
    EXPECT_TRUE(outbox_.closed.empty());
    EXPECT_TRUE(realm.contains(first));
}

TEST_F(RealmSessionsTest, TrainingFollowsTheSeqProtocol) {
    static_cast<void>(sessions());
    const auto id = enter_game(first);

    common::Train train;
    train.set_seq(1);
    request(first, train, 3);
    auto result = common::decode_train_result(reply(first, 3));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->character_id(), id);
    EXPECT_EQ(result->exp(), 10U);
    EXPECT_EQ(result->level(), 1U);
    EXPECT_EQ(result->seq(), 1U);
    EXPECT_FALSE(result->replayed());
    EXPECT_EQ(store_.stored(id)->exp, 10U);

    // 重放上一个 seq:不再计算,回当前状态并标 replayed。
    request(first, train, 4);
    result = common::decode_train_result(reply(first, 4));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exp(), 10U);
    EXPECT_EQ(result->seq(), 1U);
    EXPECT_TRUE(result->replayed());
    EXPECT_EQ(store_.stored(id)->exp, 10U);

    train.set_seq(3);
    request(first, train, 5);
    EXPECT_EQ(error_code(reply(first, 5)), common::edge_error_invalid_training_seq);
    train.set_seq(0);
    request(first, train, 6);
    EXPECT_EQ(error_code(reply(first, 6)), common::edge_error_invalid_training_seq);

    train.set_seq(2);
    request(first, train, 7);
    result = common::decode_train_result(reply(first, 7));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exp(), 20U);
    EXPECT_EQ(result->seq(), 2U);
}

TEST_F(RealmSessionsTest, SeqZeroBeforeAnyTrainingReplaysCurrentState) {
    static_cast<void>(sessions());
    const auto id = enter_game(first);

    common::Train train;
    train.set_seq(0);
    request(first, train, 3);
    const auto result = common::decode_train_result(reply(first, 3));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exp(), 0U);
    EXPECT_EQ(result->level(), 1U);
    EXPECT_EQ(result->seq(), 0U);
    EXPECT_TRUE(result->replayed());
    EXPECT_EQ(store_.stored(id)->exp, 0U);
}

TEST_F(RealmSessionsTest, MaxLevelRejectsTraining) {
    store_.add(account, {.character_id = 5, .name = "Max", .exp = 900,
                         .last_training_seq = 90});
    auto& realm = sessions();
    realm.enter(first, account);
    common::SelectCharacter select;
    select.set_character_id(5);
    request(first, select, 1);
    const auto selected = common::decode_character_selected(reply(first, 1));
    ASSERT_TRUE(selected.has_value());
    EXPECT_EQ(selected->character().level(), 10U);
    EXPECT_EQ(selected->training_seq(), 90U);

    common::Train train;
    train.set_seq(91);
    request(first, train, 2);
    EXPECT_EQ(error_code(reply(first, 2)), common::edge_error_max_level);
    EXPECT_EQ(store_.stored(5)->last_training_seq, 90U);
}

TEST_F(RealmSessionsTest, PipelinedRequestsRunInArrivalOrder) {
    auto& realm = sessions();
    realm.enter(first, account);
    store_.add(account, {.character_id = 5, .name = "Hero"});
    common::SelectCharacter select;
    select.set_character_id(5);
    common::Train train;
    train.set_seq(1);
    // 同一帧内连发:训练须等选择完成后才按游戏中阶段处理。
    request(first, select, 1);
    request(first, train, 2);
    EXPECT_TRUE(common::decode_character_selected(reply(first, 1)));
    const auto result = common::decode_train_result(reply(first, 2));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->exp(), 10U);
}

TEST_F(RealmSessionsTest, DisplacementNotifiesAndClosesOldSession) {
    static_cast<void>(sessions());
    const auto id = enter_game(first);

    // 旧会话的训练写停在工作线程里时,新会话入场。
    store_.close_gate();
    common::Train train;
    train.set_seq(1);
    request(first, train, 3);
    sessions_->enter(second, account);
    EXPECT_FALSE(sessions_->contains(first));
    EXPECT_EQ(outbox_.closed.count(first.value), 1U);
    const auto replies = for_session(first);
    ASSERT_EQ(replies.size(), 3U);
    EXPECT_TRUE(common::decode_realm_session_displaced(*replies[2]));
    EXPECT_EQ(common::realm_request_id(*replies[2]), 0U);
    EXPECT_EQ(sessions_->phase(second), RealmSessionPhase::Selecting);

    // 在途写照常落库,旧会话的回包被丢弃。
    store_.open_gate();
    request(second, common::ListCharacters{}, 1);
    const auto list = common::decode_character_list(reply(second, 1));
    ASSERT_TRUE(list.has_value());
    ASSERT_EQ(list->characters_size(), 1);
    EXPECT_EQ(store_.stored(id)->exp, 10U);
    EXPECT_EQ(for_session(first).size(), 3U);
}

TEST_F(RealmSessionsTest, UnavailableDataKeepsSessionOpen) {
    auto& realm = sessions();
    realm.enter(first, account);
    store_.set_unavailable(true);
    request(first, common::ListCharacters{}, 1);
    EXPECT_EQ(
        error_code(reply(first, 1)), common::edge_error_realm_data_unavailable);
    store_.set_unavailable(false);
    request(first, common::ListCharacters{}, 2);
    EXPECT_TRUE(common::decode_character_list(reply(first, 2)));
    EXPECT_TRUE(outbox_.closed.empty());
}

TEST_F(RealmSessionsTest, FullDataPoolThrottlesWithRetryAfter) {
    RealmConfig config;
    config.data_workers = 1;
    config.data_capacity = 1;
    config.retry_after = 3s;
    auto& realm = sessions(config);
    realm.enter(first, account);
    realm.enter(second, 43);
    store_.close_gate();
    request(first, common::ListCharacters{}, 1);
    request(second, common::ListCharacters{}, 2);
    const auto& throttled = reply(second, 1);
    const auto error = common::decode_edge_error(throttled);
    ASSERT_TRUE(error.has_value());
    EXPECT_EQ(error->code(), static_cast<std::uint32_t>(common::edge_error_throttled));
    EXPECT_EQ(error->retry_after_seconds(), 3U);
    store_.open_gate();
    EXPECT_TRUE(common::decode_character_list(reply(first, 1)));
    EXPECT_TRUE(realm.contains(second));
}

TEST_F(RealmSessionsTest, OverlongPerSessionBacklogIsThrottled) {
    RealmConfig config;
    config.max_pending_per_session = 2;  // 在途 + 排队
    auto& realm = sessions(config);
    realm.enter(first, account);
    store_.close_gate();
    request(first, common::ListCharacters{}, 1);  // 在途
    request(first, common::ListCharacters{}, 2);  // 排队
    request(first, common::ListCharacters{}, 3);  // 超出
    EXPECT_EQ(error_code(reply(first, 1)), common::edge_error_throttled);
    EXPECT_EQ(common::edge_request_id(reply(first, 1)), 3U);
    store_.open_gate();
    EXPECT_EQ(common::realm_request_id(reply(first, 2)), 1U);
    EXPECT_EQ(common::realm_request_id(reply(first, 3)), 2U);
}

/// 规则在部分经验区间报错(启动试调发现不了):该请求回 3010,会话与
/// 阶段保持,其他请求照常。
TEST_F(RealmSessionsTest, RuleErrorsAnswerUnavailableAndKeepSession) {
    const auto path = std::filesystem::temp_directory_path() /
                      "realm_sessions_faulty_rule.lua";
    std::ofstream(path) << R"lua(
return {
    level = function(exp)
        if exp >= 500 then error("level boom") end
        return 1
    end,
    train = function(exp)
        if exp >= 10 then error("train boom") end
        return exp + 10
    end,
}
)lua";
    TrainingRule faulty(path);
    std::filesystem::remove(path);
    auto& realm = sessions_with(faulty);
    store_.add(account, {.character_id = 5, .name = "Broken", .exp = 500});
    const auto id = enter_game(first);

    // enter_game 已进入游戏中;换新会话从选角开始。选择一个等级算不出来
    // 的角色:3010,仍在选角阶段。
    realm.enter(second, account);
    common::SelectCharacter select;
    select.set_character_id(5);
    request(second, select, 1);
    EXPECT_EQ(
        error_code(reply(second, 1)), common::edge_error_realm_data_unavailable);
    EXPECT_EQ(realm.phase(second), RealmSessionPhase::Selecting);

    select.set_character_id(id);
    request(second, select, 2);
    EXPECT_TRUE(common::decode_character_selected(reply(second, 2)));
    common::Train train;
    train.set_seq(1);
    request(second, train, 3);
    EXPECT_TRUE(common::decode_train_result(reply(second, 3)));
    train.set_seq(2);
    request(second, train, 4);
    EXPECT_EQ(
        error_code(reply(second, 4)), common::edge_error_realm_data_unavailable);
    EXPECT_TRUE(realm.contains(second));
    EXPECT_EQ(store_.stored(id)->exp, 10U);
}

TEST_F(RealmSessionsTest, NonRealmOrServerMessagesAreNotHandled) {
    auto& realm = sessions();
    realm.enter(first, account);
    EXPECT_FALSE(realm.handle(first, common::encode(common::HeartbeatRequest{}, 1)));
    EXPECT_FALSE(realm.handle(first, common::encode(common::CharacterList{}, 1)));
    EXPECT_FALSE(realm.handle(second, common::encode(common::ListCharacters{}, 1)));
    std::vector<std::byte> garbage(3, std::byte{0x7F});
    EXPECT_FALSE(realm.handle(first, garbage));
}

}  // namespace
