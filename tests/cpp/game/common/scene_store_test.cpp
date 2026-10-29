#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/test_support/mongod_process.hpp"

#include <gtest/gtest.h>

#include <array>
#include <barrier>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <optional>
#include <string>
#include <thread>

namespace realm::game::common {
namespace {

const SceneCharacterKey key{42, 1, 7001};
const SceneOwner owner{"realm-start-1", 11, "scene-a", "scene-start-1"};

void ready_instance(
    MongoPlayerDataStore& store, const SceneShardOwner& instance) {
    const auto expected = store.scene_instance_state(instance.instance_id);
    ASSERT_TRUE(store.compare_exchange_scene_instance(
        expected, {instance.instance_id, instance.incarnation, false}));
}

void provision(MongoPlayerDataStore& store) {
    store.provision_account(
        {.account_id = key.account_id,
         .account_name = "player",
         .credential = "secret"});
    store.provision_character(
        {.character_id = key.character_id,
         .account_id = key.account_id,
         .realm_id = key.realm_id,
         .name = "Ranger"});
}
SceneWrite enter_write() {
    return {
        .character = key,
        .owner = owner,
        .command = {SceneCommandType::Enter, 1},
        .location = {1, 0, 0}};
}
SceneWrite move_write(std::uint64_t seq = 2, std::int32_t x = 10) {
    return {
        .character = key,
        .expected_epoch = 1,
        .expected_owner = owner,
        .owner = owner,
        .command = {SceneCommandType::Move, seq, x, 0},
        .location = {1, x, 0}};
}

class SceneStoreTest : public ::testing::Test {
protected:
    void TearDown() override { mongod.drop_database(database); }
    MongoPlayerDataStore open() const {
        MongoPlayerDataOptions options;
        options.credential_hash_cost = CredentialHashCost::Minimum;
        return MongoPlayerDataStore(mongod.uri(), database, options);
    }
    test_support::MongodProcess& mongod{test_support::MongodProcess::shared()};
    std::string database{test_support::MongodProcess::fresh_database()};
};

class SceneShardStoreTest : public SceneStoreTest {};

TEST_F(SceneShardStoreTest, ConcurrentAllocationHasOneDurableOwner) {
    auto first = open();
    auto second = open();
    std::barrier start(2);
    std::array<bool, 2> won{};
    std::array<std::exception_ptr, 2> errors{};
    const std::array<SceneShardRecord, 2> desired{
        {{1, 1, 1, SceneShardOwner{"scene-a", "boot-a"}, false},
         {1, 1, 1, SceneShardOwner{"scene-b", "boot-b"}, false}}};
    ready_instance(first, *desired[0].owner);
    ready_instance(first, *desired[1].owner);
    auto claim = [&](std::size_t i, MongoPlayerDataStore& store) {
        start.arrive_and_wait();
        try {
            won[i] = store.compare_exchange_shard(std::nullopt, desired[i]);
        } catch (...) {
            errors[i] = std::current_exception();
        }
    };
    std::jthread a([&] {
        claim(0, first);
    });
    std::jthread b([&] {
        claim(1, second);
    });
    a.join();
    b.join();
    ASSERT_FALSE(errors[0]);
    ASSERT_FALSE(errors[1]);
    EXPECT_NE(won[0], won[1]);
    EXPECT_EQ(first.shard_state(1, 1), std::optional(desired[won[0] ? 0 : 1]));
    EXPECT_EQ(
        mongod.eval(database, "print(db.scene_shards.countDocuments())"), "1");
    EXPECT_FALSE(first.compare_exchange_shard(std::nullopt, desired[0]));
}

TEST_F(
    SceneShardStoreTest, DrainingFencesNewAdmissionButKeepsExistingMovement) {
    auto store = open();
    provision(store);
    SceneShardRecord shard{
        1,
        1,
        1,
        SceneShardOwner{owner.scene_instance_id, owner.scene_incarnation},
        false};
    ready_instance(store, *shard.owner);
    ASSERT_TRUE(store.compare_exchange_shard(std::nullopt, shard));
    auto enter = enter_write();
    enter.owner.shard_version = 1;
    ASSERT_EQ(store.commit_scene(enter).outcome, SceneWriteOutcome::Committed);
    auto draining = shard;
    draining.draining = true;
    ASSERT_TRUE(store.compare_exchange_shard(shard, draining));
    EXPECT_EQ(store.shard_state(1, 1)->version, 1);
    auto move = move_write();
    move.owner.shard_version = 1;
    move.expected_owner = move.owner;
    EXPECT_EQ(store.commit_scene(move).outcome, SceneWriteOutcome::Committed);
    const auto before = store.scene_state(key);
    auto reenter = enter;
    reenter.expected_epoch = 1;
    reenter.expected_owner = enter.owner;
    reenter.command.seq = 3;
    reenter.location = *before->confirmed_location;
    EXPECT_EQ(
        store.commit_scene(reenter).outcome, SceneWriteOutcome::StaleBinding);
    EXPECT_EQ(store.scene_state(key), before);
    EXPECT_THROW(
        static_cast<void>(store.compare_exchange_shard(draining, shard)),
        PlayerDataError);
}

TEST_F(SceneShardStoreTest, NewCarrierFencesOldWriteAndPreservesLogicalShard) {
    auto store = open();
    provision(store);
    const SceneShardRecord first{
        1,
        1,
        1,
        SceneShardOwner{owner.scene_instance_id, owner.scene_incarnation},
        false};
    ready_instance(store, *first.owner);
    ready_instance(store, {"scene-b", "boot-b"});
    ASSERT_TRUE(store.compare_exchange_shard(std::nullopt, first));
    auto enter = enter_write();
    enter.owner.shard_version = 1;
    ASSERT_EQ(store.commit_scene(enter).outcome, SceneWriteOutcome::Committed);
    auto next = first;
    next.version = 2;
    next.owner = SceneShardOwner{"scene-b", "boot-b"};
    ASSERT_TRUE(store.compare_exchange_shard(first, next));
    auto old = move_write();
    old.owner = enter.owner;
    old.expected_owner = enter.owner;
    EXPECT_EQ(store.commit_scene(old).outcome, SceneWriteOutcome::StaleBinding);
    EXPECT_EQ(store.scene_state(key)->last_result->command.seq, 1);
    auto recovery = enter;
    recovery.expected_epoch = 1;
    recovery.expected_owner = enter.owner;
    recovery.owner.scene_instance_id = "scene-b";
    recovery.owner.scene_incarnation = "boot-b";
    recovery.owner.shard_version = 2;
    recovery.command.seq = 2;
    const auto restored = store.commit_scene(recovery);
    ASSERT_EQ(restored.outcome, SceneWriteOutcome::Committed);
    EXPECT_EQ(
        restored.state->confirmed_location,
        std::optional(SceneLocation{1, 0, 0}));
    EXPECT_EQ(restored.state->binding_epoch, 2);
    EXPECT_EQ(restored.state->owner->shard_version, 2);
    EXPECT_FALSE(store.compare_exchange_shard(first, next));
}

TEST_F(SceneShardStoreTest, ReleaseCannotReuseVersionOrSkipDraining) {
    auto store = open();
    SceneShardRecord first{
        1, 1, 1, SceneShardOwner{"scene-a", "boot-a"}, false};
    ready_instance(store, *first.owner);
    ASSERT_TRUE(store.compare_exchange_shard(std::nullopt, first));
    SceneShardRecord released{1, 1, 2, std::nullopt, false};
    EXPECT_THROW(
        static_cast<void>(store.compare_exchange_shard(first, released)),
        PlayerDataError);
    auto draining = first;
    draining.draining = true;
    ASSERT_TRUE(store.compare_exchange_shard(first, draining));
    ASSERT_TRUE(store.compare_exchange_shard(draining, released));
    EXPECT_FALSE(store.compare_exchange_shard(std::nullopt, first));
    auto next = first;
    next.version = 3;
    ASSERT_TRUE(store.compare_exchange_shard(released, next));
    EXPECT_EQ(store.shard_state(1, 1)->version, 3);
    auto skipped = next;
    skipped.version = 5;
    skipped.owner = SceneShardOwner{"scene-b", "boot-b"};
    EXPECT_THROW(
        static_cast<void>(store.compare_exchange_shard(next, skipped)),
        PlayerDataError);
}

TEST_F(SceneShardStoreTest, InstanceDrainAndAllocationAreAtomicallyFenced) {
    auto first = open();
    auto second = open();
    const SceneInstanceState ready{"scene-a", "boot-a", false};
    ASSERT_TRUE(first.compare_exchange_scene_instance(std::nullopt, ready));
    auto draining = ready;
    draining.draining = true;
    const SceneShardRecord desired{
        1, 1, 1, SceneShardOwner{"scene-a", "boot-a"}, false};
    std::barrier start(2);
    bool claimed = false, drained = false;
    std::array<std::exception_ptr, 2> errors{};
    std::jthread a([&] {
        start.arrive_and_wait();
        try {
            claimed = first.compare_exchange_shard(std::nullopt, desired);
        } catch (...) {
            errors[0] = std::current_exception();
        }
    });
    std::jthread b([&] {
        start.arrive_and_wait();
        try {
            drained = second.compare_exchange_scene_instance(ready, draining);
        } catch (...) {
            errors[1] = std::current_exception();
        }
    });
    a.join();
    b.join();
    ASSERT_FALSE(errors[0]);
    ASSERT_FALSE(errors[1]);
    ASSERT_TRUE(drained);
    EXPECT_EQ(first.scene_instance_state("scene-a"), std::optional(draining));
    const auto shard = first.shard_state(1, 1);
    EXPECT_EQ(shard.has_value(), claimed);
    if (shard) EXPECT_TRUE(shard->draining);
    auto late = desired;
    late.shard_id = 2;
    EXPECT_FALSE(first.compare_exchange_shard(std::nullopt, late));
    EXPECT_FALSE(first.shard_state(1, 2));
    EXPECT_THROW(
        static_cast<void>(
            first.compare_exchange_scene_instance(draining, ready)),
        PlayerDataError);
}

TEST_F(SceneStoreTest, ConcurrentLazyInitializationPreservesOneRecord) {
    auto first = open();
    auto second = open();
    provision(first);
    std::barrier start(2);
    std::array<std::optional<SceneStateRecord>, 2> values;
    std::array<std::exception_ptr, 2> errors;
    auto run = [&](std::size_t i, MongoPlayerDataStore& store) {
        start.arrive_and_wait();
        try {
            values[i] = store.initialize_scene(key);
        } catch (...) {
            errors[i] = std::current_exception();
        }
    };
    std::jthread a([&] {
        run(0, first);
    });
    std::jthread b([&] {
        run(1, second);
    });
    a.join();
    b.join();
    EXPECT_FALSE(errors[0]);
    EXPECT_FALSE(errors[1]);
    ASSERT_TRUE(values[0]);
    ASSERT_TRUE(values[1]);
    EXPECT_EQ(values[0], values[1]);
    EXPECT_EQ(values[0]->binding_epoch, 0U);
    EXPECT_FALSE(values[0]->owner);
    EXPECT_EQ(
        mongod.eval(database, "print(db.character_scenes.countDocuments())"),
        "1");
    EXPECT_EQ(
        first.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    EXPECT_EQ(second.initialize_scene(key)->binding_epoch, 1U);
}

TEST_F(SceneStoreTest, NonOwnedCharacterNeverInitializesOrLeaksSceneData) {
    auto store = open();
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    for (auto other :
         {SceneCharacterKey{43, 1, 7001},
          SceneCharacterKey{42, 2, 7001},
          SceneCharacterKey{42, 1, 9999}}) {
        EXPECT_FALSE(store.initialize_scene(other));
        EXPECT_FALSE(store.scene_state(other));
        auto write = enter_write();
        write.character = other;
        EXPECT_EQ(
            store.commit_scene(write).outcome, SceneWriteOutcome::NotOwned);
        EXPECT_FALSE(store.revoke_scene(other, 1, owner));
    }
    EXPECT_EQ(
        mongod.eval(database, "print(db.character_scenes.countDocuments())"),
        "1");
}

TEST_F(SceneStoreTest, EnterAndMoveReplayOnlySameBusinessParameters) {
    auto store = open();
    provision(store);
    const auto enter = enter_write();
    ASSERT_EQ(store.commit_scene(enter).outcome, SceneWriteOutcome::Committed);
    EXPECT_EQ(store.commit_scene(enter).outcome, SceneWriteOutcome::Replayed);
    const auto move = move_write();
    ASSERT_EQ(store.commit_scene(move).outcome, SceneWriteOutcome::Committed);
    EXPECT_EQ(store.commit_scene(move).outcome, SceneWriteOutcome::Replayed);
    EXPECT_EQ(
        store.commit_scene(move_write(2, 20)).outcome,
        SceneWriteOutcome::InvalidSequence);
    EXPECT_EQ(
        store.commit_scene(move_write(4)).outcome,
        SceneWriteOutcome::InvalidSequence);
    EXPECT_EQ(
        store.commit_scene(enter).outcome, SceneWriteOutcome::StaleBinding);
    auto reopened = open();
    const auto state = reopened.scene_state(key);
    ASSERT_TRUE(state);
    EXPECT_EQ(state->last_result->command.seq, 2U);
    EXPECT_EQ(state->confirmed_location->x, 10);
    EXPECT_EQ(
        mongod.eval(
            database,
            "print(db.character_scenes.findOne().last_scene_seq.toString())"),
        "2");
}

TEST_F(SceneStoreTest, RacingMovesHaveExactlyOneWinner) {
    auto store = open();
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    std::barrier start(2);
    std::array<SceneWriteResult, 2> results;
    std::array<std::exception_ptr, 2> errors;
    auto run = [&](std::size_t i) {
        start.arrive_and_wait();
        try {
            results[i] = store.commit_scene(
                move_write(2, static_cast<std::int32_t>(10 + i)));
        } catch (...) {
            errors[i] = std::current_exception();
        }
    };
    std::jthread a([&] {
        run(0);
    });
    std::jthread b([&] {
        run(1);
    });
    a.join();
    b.join();
    ASSERT_FALSE(errors[0]);
    ASSERT_FALSE(errors[1]);
    EXPECT_EQ(
        static_cast<int>(results[0].outcome == SceneWriteOutcome::Committed) +
            static_cast<int>(
                results[1].outcome == SceneWriteOutcome::Committed),
        1);
    const auto state = store.scene_state(key);
    ASSERT_TRUE(state);
    EXPECT_EQ(state->last_result->command.seq, 2U);
    EXPECT_TRUE(
        state->confirmed_location->x == 10 ||
        state->confirmed_location->x == 11);
}

TEST_F(SceneStoreTest, RebindingFencesOldRealmSceneAndCleanup) {
    auto store = open();
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    auto rebind = enter_write();
    rebind.expected_epoch = 1;
    rebind.expected_owner = owner;
    rebind.owner.realm_incarnation = "realm-start-2";
    rebind.owner.session_binding = 12;
    rebind.owner.scene_incarnation = "scene-start-2";
    rebind.command.seq = 2;
    ASSERT_EQ(store.commit_scene(rebind).outcome, SceneWriteOutcome::Committed);
    EXPECT_EQ(store.scene_state(key)->binding_epoch, 2U);
    EXPECT_EQ(
        store.commit_scene(move_write(3)).outcome,
        SceneWriteOutcome::StaleBinding);
    EXPECT_FALSE(store.revoke_scene(key, 1, owner));
    auto impostor = move_write(3);
    impostor.expected_epoch = 2;
    impostor.expected_owner = rebind.owner;
    impostor.owner = rebind.owner;
    impostor.expected_owner->scene_incarnation = owner.scene_incarnation;
    EXPECT_EQ(
        store.commit_scene(impostor).outcome, SceneWriteOutcome::StaleBinding);
    EXPECT_TRUE(store.revoke_scene(key, 2, rebind.owner));
    EXPECT_FALSE(store.revoke_scene(key, 2, rebind.owner));
    const auto revoked = store.scene_state(key);
    ASSERT_TRUE(revoked);
    EXPECT_EQ(revoked->binding_epoch, 3U);
    EXPECT_FALSE(revoked->owner);
    EXPECT_EQ(revoked->last_result->command.seq, 2U);
    EXPECT_EQ(
        resolve_scene_write(rebind, revoked), SceneWriteResolution::Committed);
    EXPECT_EQ(
        store.commit_scene(rebind).outcome, SceneWriteOutcome::StaleBinding);
}

TEST_F(SceneStoreTest, TransferChangesBindingAndLocationAtomically) {
    auto store = open();
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    auto transfer = move_write();
    transfer.command = {SceneCommandType::Transfer, 2, 0, 0, 2};
    transfer.location = {2, 5, 5};
    transfer.owner.scene_instance_id = "scene-b";
    transfer.owner.scene_incarnation = "scene-b-start-1";
    const auto committed = store.commit_scene(transfer);
    ASSERT_EQ(committed.outcome, SceneWriteOutcome::Committed);
    EXPECT_EQ(committed.state->binding_epoch, 2U);
    EXPECT_EQ(committed.state->confirmed_location, transfer.location);
    EXPECT_EQ(committed.state->last_result->owner, transfer.owner);
    EXPECT_EQ(
        store.commit_scene(transfer).outcome, SceneWriteOutcome::Replayed);
    EXPECT_EQ(
        store.commit_scene(move_write(3)).outcome,
        SceneWriteOutcome::StaleBinding);
}

TEST_F(SceneStoreTest, ReentryPreservesLastLocationAndDoesNotResetSequence) {
    auto store = open();
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    ASSERT_EQ(
        store.commit_scene(move_write()).outcome, SceneWriteOutcome::Committed);
    ASSERT_TRUE(store.revoke_scene(key, 1, owner));
    auto reentry = enter_write();
    reentry.expected_epoch = 2;
    reentry.command.seq = 3;
    reentry.location = {1, 10, 0};
    reentry.owner.scene_incarnation = "scene-restarted";
    auto reopened = open();
    const auto committed = reopened.commit_scene(reentry);
    ASSERT_EQ(committed.outcome, SceneWriteOutcome::Committed);
    EXPECT_EQ(committed.state->binding_epoch, 3U);
    EXPECT_EQ(committed.state->confirmed_location, reentry.location);
    EXPECT_EQ(committed.state->last_result->command.seq, 3U);
    EXPECT_EQ(
        reopened.commit_scene(move_write()).outcome,
        SceneWriteOutcome::StaleBinding);
    EXPECT_FALSE(reopened.revoke_scene(key, 1, owner));
}

TEST_F(SceneStoreTest, PreservesFullWidthDomainIdsInSceneRecords) {
    auto store = open();
    const SceneCharacterKey wide{
        0xF000'0000'0000'0001ULL, 0xFFFF'FFFFU, 0xE000'0000'0000'0002ULL};
    store.provision_account(
        {.account_id = wide.account_id,
         .account_name = "wide",
         .credential = "secret"});
    store.provision_character(
        {.character_id = wide.character_id,
         .account_id = wide.account_id,
         .realm_id = wide.realm_id,
         .name = "Wide"});
    auto write = enter_write();
    write.character = wide;
    write.owner.session_binding = 0xC000'0000'0000'0003ULL;
    write.location.shard_id = 0xD000'0000'0000'0004ULL;
    ASSERT_EQ(store.commit_scene(write).outcome, SceneWriteOutcome::Committed);
    const auto stored = store.scene_state(wide);
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->character, wide);
    EXPECT_EQ(stored->owner, write.owner);
    EXPECT_EQ(stored->confirmed_location, write.location);
    auto transfer = write;
    transfer.expected_epoch = 1;
    transfer.expected_owner = write.owner;
    transfer.command = {
        SceneCommandType::Transfer, 2, 0, 0, write.location.shard_id + 1};
    transfer.location.shard_id++;
    EXPECT_EQ(
        store.commit_scene(transfer).outcome, SceneWriteOutcome::Committed);
    EXPECT_EQ(
        store.scene_state(wide)->last_result->command.target_shard_id,
        transfer.location.shard_id);
}

TEST_F(SceneStoreTest, InvalidTransitionsNeverChangePersistedState) {
    auto store = open();
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    const auto before = store.scene_state(key);
    auto teleport = enter_write();
    teleport.expected_epoch = 1;
    teleport.expected_owner = owner;
    teleport.command.seq = 2;
    teleport.location.x = 10;
    EXPECT_EQ(
        store.commit_scene(teleport).outcome,
        SceneWriteOutcome::InvalidCommand);
    auto move = move_write();
    move.owner.session_binding++;
    EXPECT_EQ(
        store.commit_scene(move).outcome, SceneWriteOutcome::InvalidCommand);
    move = move_write();
    move.location.shard_id = 2;
    EXPECT_EQ(
        store.commit_scene(move).outcome, SceneWriteOutcome::InvalidCommand);
    move = move_write();
    move.location.x = -1;
    EXPECT_THROW(static_cast<void>(store.commit_scene(move)), PlayerDataError);
    EXPECT_EQ(store.scene_state(key), before);
}

TEST_F(SceneStoreTest, TrainingAndMovementDoNotOverwriteEachOther) {
    auto store = open();
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    std::jthread training([&] {
        const auto result = store.record_training({42, 1, 7001, 1, 10});
        EXPECT_TRUE(result && result->committed);
    });
    EXPECT_EQ(
        store.commit_scene(move_write()).outcome, SceneWriteOutcome::Committed);
    training.join();
    EXPECT_EQ(store.scene_state(key)->confirmed_location->x, 10);
    EXPECT_EQ(store.realm_character(42, 1, 7001)->exp, 10U);
    EXPECT_EQ(store.realm_character(42, 1, 7001)->last_training_seq, 1U);
}

TEST_F(SceneStoreTest, EpochExhaustionDoesNotWrapOrChangeBinding) {
    auto store = open();
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    static_cast<void>(mongod.eval(
        database,
        "db.character_scenes.updateOne({}, {$set: {binding_epoch: NumberLong('9223372036854775807'), 'last_result.binding_epoch': NumberLong('9223372036854775807')}})"));
    auto write = enter_write();
    write.expected_epoch = scene_counter_max;
    write.expected_owner = owner;
    write.command.seq = 2;
    EXPECT_THROW(static_cast<void>(store.commit_scene(write)), PlayerDataError);
    EXPECT_THROW(
        static_cast<void>(store.revoke_scene(key, scene_counter_max, owner)),
        PlayerDataError);
    EXPECT_EQ(store.scene_state(key)->binding_epoch, scene_counter_max);
}

#ifdef __linux__
// Linux 安装器把工具放在仓库 .tools，不保证加入 PATH；macOS 使用 Homebrew。
// 真实启动和 mongosh 回读覆盖同一个调用缝，不能只检查 CMake 字符串。
class ScopedToolEnvironment final {
public:
    ScopedToolEnvironment(const char* name, const char* value)
        : name_(name) {
        if (const auto* previous = std::getenv(name)) previous_ = previous;
        if (value)
            static_cast<void>(::setenv(name_, value, 1));
        else
            static_cast<void>(::unsetenv(name_));
    }
    ~ScopedToolEnvironment() {
        if (previous_)
            static_cast<void>(::setenv(name_, previous_->c_str(), 1));
        else
            static_cast<void>(::unsetenv(name_));
    }

private:
    const char* name_;
    std::optional<std::string> previous_;
};

TEST(SceneToolDiscoveryTest, StartsAndReadsRepositoryToolsOutsidePath) {
    ScopedToolEnvironment path("PATH", "/usr/bin:/bin");
    ScopedToolEnvironment mongod_override("REALMMESH_MONGOD_BINARY", nullptr);
    ScopedToolEnvironment mongosh_override("REALMMESH_MONGOSH_BINARY", nullptr);
    test_support::MongodProcess mongod;
    EXPECT_EQ(
        mongod.eval(
            test_support::MongodProcess::fresh_database(), "print(6 * 7)"),
        "42");
}
#endif

// 独占的真实副本集启用测试命令，故障点绝不作用于共享 CTest 后端。
TEST(SceneCommitRecoveryTest, FailedDurabilityAcknowledgementRequiresReadback) {
    test_support::MongodProcess mongod(
        test_support::MongodProcess::TestCommands::Enabled);
    const auto database = test_support::MongodProcess::fresh_database();
    MongoPlayerDataOptions options;
    options.credential_hash_cost = CredentialHashCost::Minimum;
    MongoPlayerDataStore store(mongod.uri(), database, options);
    provision(store);
    ASSERT_EQ(
        store.commit_scene(enter_write()).outcome,
        SceneWriteOutcome::Committed);
    const auto old = store.scene_state(key);
    const auto move = move_write();
    EXPECT_EQ(resolve_scene_write(move, old), SceneWriteResolution::Unknown);
    static_cast<void>(mongod.eval("admin", R"js(
const r = db.runCommand({configureFailPoint: 'failCommand', mode: {times: 1}, data: {
  failCommands: ['findAndModify'], writeConcernError: {code: 64, codeName: 'WriteConcernFailed', errmsg: 'injected durability acknowledgement failure'}
}}); if (!r.ok) throw new Error(JSON.stringify(r));
)js"));
    std::string failure;
    try {
        static_cast<void>(store.commit_scene(move));
        FAIL() << "injected acknowledgement failure was not reported";
    } catch (const SceneWriteUncertain& error) {
        failure = error.what();
    }
    SCOPED_TRACE(failure);
    // 注入的 writeConcernError 不等待多数提交；一次旧值不能证明未提交。
    // 在截止内继续 majority 回读，直到精确结果可见，不能重置序号。
    auto observed = store.scene_state(key);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds{3};
    while (resolve_scene_write(move, observed) ==
               SceneWriteResolution::Unknown &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
        observed = store.scene_state(key);
    }
    EXPECT_EQ(
        resolve_scene_write(move, observed), SceneWriteResolution::Committed);
    ASSERT_TRUE(observed);
    EXPECT_EQ(observed->confirmed_location->x, 10);
    EXPECT_EQ(store.commit_scene(move).outcome, SceneWriteOutcome::Replayed);
    EXPECT_EQ(
        mongod.eval(
            database,
            "print(db.character_scenes.findOne().last_scene_seq.toString())"),
        "2");
}

}  // namespace
}  // namespace realm::game::common
