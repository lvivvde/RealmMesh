#include "realmmesh/game/scene/scene_shard.hpp"
#include <gtest/gtest.h>

namespace realm::game::scene {
namespace {
namespace internal = ::realmmesh::protocol::scene_internal::v1;
common::SceneRequest request(
    internal::Operation op,
    std::uint64_t character = 10,
    std::uint64_t epoch = 1,
    std::uint64_t seq = 1,
    int x = 0,
    int y = 0) {
    common::SceneRequest r;
    r.set_realm_incarnation("realm-boot");
    r.set_scene_instance_id("scene");
    r.set_scene_incarnation("boot");
    r.set_operation(op);
    if (op == internal::OPERATION_SNAPSHOT) {
        r.set_shard_id(1);
        return r;
    }
    auto* route = r.mutable_route();
    route->set_realm_id(1);
    route->set_character_id(character);
    route->set_binding_epoch(epoch);
    route->set_session_binding(epoch);
    route->set_shard_id(1);
    if (op == internal::OPERATION_REMOVE) return r;
    r.set_scene_seq(seq);
    r.set_command_digest(std::string(32, static_cast<char>(seq)));
    if (op != internal::OPERATION_ACTIVATE) {
        r.mutable_location()->set_shard_id(1);
        r.mutable_location()->set_x(x);
        r.mutable_location()->set_y(y);
    }
    return r;
}
void activate(
    SceneShard& shard,
    std::uint64_t character = 10,
    std::uint64_t epoch = 1,
    std::uint64_t connection = 1) {
    ASSERT_EQ(
        shard
            .handle(
                connection,
                request(internal::OPERATION_PREPARE, character, epoch))
            .status(),
        internal::STATUS_OK);
    ASSERT_EQ(
        shard
            .handle(
                connection,
                request(internal::OPERATION_ACTIVATE, character, epoch))
            .status(),
        internal::STATUS_OK);
}
TEST(SceneShardTest, PreparedAndProposedPositionsAreInvisibleUntilConfirmed) {
    SceneShard shard("scene", "boot");
    const auto prepare = request(internal::OPERATION_PREPARE);
    EXPECT_EQ(shard.handle(1, prepare).status(), internal::STATUS_OK);
    EXPECT_EQ(
        shard.handle(1, request(internal::OPERATION_SNAPSHOT))
            .snapshot()
            .members_size(),
        0);
    EXPECT_TRUE(shard.drain_events().empty());
    auto joined = shard.handle(1, request(internal::OPERATION_ACTIVATE));
    ASSERT_EQ(joined.snapshot().members_size(), 1);
    EXPECT_EQ(joined.snapshot().event_seq(), 1);
    EXPECT_EQ(shard.drain_events().size(), 1);
    auto move = request(internal::OPERATION_PROPOSE_MOVE, 10, 1, 2, 60, 40);
    EXPECT_EQ(shard.handle(1, move).status(), internal::STATUS_OK);
    EXPECT_EQ(
        shard.handle(1, request(internal::OPERATION_SNAPSHOT))
            .snapshot()
            .members(0)
            .location()
            .x(),
        0);
    EXPECT_TRUE(shard.drain_events().empty());
    move.set_operation(internal::OPERATION_CONFIRM);
    EXPECT_EQ(shard.handle(1, move).status(), internal::STATUS_OK);
    auto events = shard.drain_events();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].event_seq(), 2);
    EXPECT_EQ(events[0].member().location().x(), 60);
    EXPECT_EQ(shard.handle(1, move).status(), internal::STATUS_OK);
    EXPECT_TRUE(shard.drain_events().empty());
}

TEST(SceneShardTest, OwnershipVersionFencesRoutesAndSeparatesEventStreams) {
    SceneShard current("scene", "boot", 1, 2);
    auto prepare = request(internal::OPERATION_PREPARE);
    prepare.mutable_route()->set_shard_version(1);
    EXPECT_NE(current.handle(1, prepare).status(), internal::STATUS_OK);
    prepare.mutable_route()->set_shard_version(2);
    ASSERT_EQ(current.handle(1, prepare).status(), internal::STATUS_OK);
    auto activate_request = request(internal::OPERATION_ACTIVATE);
    activate_request.mutable_route()->set_shard_version(2);
    const auto activated = current.handle(1, activate_request);
    ASSERT_EQ(activated.status(), internal::STATUS_OK);
    EXPECT_EQ(activated.scene_incarnation(), "boot");
    EXPECT_EQ(activated.snapshot().shard_incarnation(), "boot:2");
    current.begin_draining();
    auto move = request(internal::OPERATION_PROPOSE_MOVE, 10, 1, 2, 10);
    move.mutable_route()->set_shard_version(2);
    EXPECT_EQ(current.handle(1, move).status(), internal::STATUS_OK);
    auto newcomer = request(internal::OPERATION_PREPARE, 20);
    newcomer.mutable_route()->set_shard_version(2);
    EXPECT_EQ(current.handle(1, newcomer).status(), internal::STATUS_DRAINING);
    SceneShard replacement("scene", "boot", 1, 3);
    auto probe = request(internal::OPERATION_SNAPSHOT);
    probe.set_shard_version(2);
    EXPECT_NE(replacement.handle(1, probe).status(), internal::STATUS_OK);
    probe.set_shard_version(3);
    const auto snapshot = replacement.handle(1, probe);
    ASSERT_EQ(snapshot.status(), internal::STATUS_OK);
    EXPECT_NE(
        snapshot.snapshot().shard_incarnation(),
        activated.snapshot().shard_incarnation());
}
TEST(
    SceneShardTest,
    RetriesDoNotPublishTwiceAndOldCleanupCannotRemoveNewBinding) {
    SceneShard shard("scene", "boot");
    activate(shard);
    static_cast<void>(shard.drain_events());
    activate(shard);
    EXPECT_TRUE(shard.drain_events().empty());
    activate(shard, 10, 2, 2);
    const auto events = shard.drain_events();
    ASSERT_EQ(events.size(), 2);
    EXPECT_EQ(events[0].kind(), common::SceneEvent::KIND_LEAVE);
    EXPECT_EQ(events[1].member().binding_epoch(), 2);
    EXPECT_EQ(
        shard.handle(1, request(internal::OPERATION_REMOVE)).status(),
        internal::STATUS_OK);
    shard.closed(1);
    EXPECT_TRUE(shard.drain_events().empty());
    EXPECT_EQ(
        shard.handle(2, request(internal::OPERATION_SNAPSHOT))
            .snapshot()
            .members_size(),
        1);
    EXPECT_EQ(
        shard.handle(1, request(internal::OPERATION_PROPOSE_MOVE, 10, 1, 2, 10))
            .status(),
        internal::STATUS_STALE_BINDING);
}
TEST(SceneShardTest, CandidateExpiryRequiresReprepareOfCommittedReceipt) {
    SceneShard shard("scene", "boot");
    activate(shard);
    static_cast<void>(shard.drain_events());
    const auto now = SceneShard::Clock::now();
    auto move = request(internal::OPERATION_PROPOSE_MOVE, 10, 1, 2, 20);
    EXPECT_EQ(shard.handle(1, move, now).status(), internal::STATUS_OK);
    move.set_operation(internal::OPERATION_CONFIRM);
    EXPECT_EQ(
        shard.handle(1, move, now + std::chrono::seconds(4)).status(),
        internal::STATUS_STALE_BINDING);
    EXPECT_TRUE(shard.drain_events().empty());
    move.set_operation(internal::OPERATION_PREPARE);
    EXPECT_EQ(shard.handle(1, move).status(), internal::STATUS_OK);
    EXPECT_EQ(
        shard.handle(1, request(internal::OPERATION_ACTIVATE, 10, 1, 2))
            .status(),
        internal::STATUS_OK);
    const auto events = shard.drain_events();
    ASSERT_EQ(events.size(), 1);
    EXPECT_EQ(events[0].kind(), common::SceneEvent::KIND_MOVE);
}
TEST(SceneShardTest, DistanceCapacityAndPendingResourcesAreBounded) {
    SceneShard shard("scene", "boot");
    activate(shard);
    EXPECT_EQ(
        shard
            .handle(
                1, request(internal::OPERATION_PROPOSE_MOVE, 10, 1, 2, 60, 41))
            .status(),
        internal::STATUS_INVALID);
    const auto now = SceneShard::Clock::now();
    for (std::uint64_t i = 11; i < 26; ++i)
        EXPECT_EQ(
            shard.handle(1, request(internal::OPERATION_PREPARE, i), now)
                .status(),
            internal::STATUS_OK);
    EXPECT_EQ(
        shard.handle(1, request(internal::OPERATION_PREPARE, 26), now).status(),
        internal::STATUS_BUSY);
    EXPECT_EQ(
        shard
            .handle(
                1,
                request(internal::OPERATION_PREPARE, 26),
                now + std::chrono::seconds(4))
            .status(),
        internal::STATUS_OK);
}
TEST(SceneShardTest, SnapshotWatermarkThenJoinMoveLeaveIsContiguous) {
    SceneShard shard("scene", "boot");
    activate(shard);
    static_cast<void>(shard.drain_events());
    const auto before =
        shard.handle(1, request(internal::OPERATION_SNAPSHOT)).snapshot();
    activate(shard, 20, 1, 2);
    auto move = request(internal::OPERATION_PROPOSE_MOVE, 20, 1, 2, 5);
    ASSERT_EQ(shard.handle(2, move).status(), internal::STATUS_OK);
    move.set_operation(internal::OPERATION_CONFIRM);
    ASSERT_EQ(shard.handle(2, move).status(), internal::STATUS_OK);
    shard.closed(2);
    auto events = shard.drain_events();
    ASSERT_EQ(events.size(), 3);
    for (std::size_t i = 0; i < events.size(); ++i)
        EXPECT_EQ(events[i].event_seq(), before.event_seq() + i + 1);
    EXPECT_EQ(events.back().kind(), common::SceneEvent::KIND_LEAVE);
}
TEST(SceneShardTest, WrongIncarnationCannotMutateAndOnlySnapshotCanDiscover) {
    SceneShard shard("scene", "boot");
    auto r = request(internal::OPERATION_PREPARE);
    r.set_scene_incarnation("discover");
    EXPECT_EQ(shard.handle(1, r).status(), internal::STATUS_STALE_BINDING);
    r = request(internal::OPERATION_SNAPSHOT);
    r.set_scene_incarnation("discover");
    EXPECT_EQ(shard.handle(1, r).scene_incarnation(), "boot");
}
TEST(SceneShardTest, ConfiguredSecondShardRejectsFirstShardRoutes) {
    SceneShard shard("scene", "boot", 2);
    auto prepare = request(internal::OPERATION_PREPARE);
    EXPECT_EQ(shard.handle(1, prepare).status(), internal::STATUS_NOT_FOUND);
    prepare.mutable_route()->set_shard_id(2);
    prepare.mutable_location()->set_shard_id(2);
    EXPECT_EQ(shard.handle(1, prepare).status(), internal::STATUS_OK);
    auto activate = request(internal::OPERATION_ACTIVATE);
    activate.mutable_route()->set_shard_id(2);
    const auto reply = shard.handle(1, activate);
    ASSERT_EQ(reply.status(), internal::STATUS_OK);
    EXPECT_EQ(reply.snapshot().shard_id(), 2);
    EXPECT_EQ(reply.snapshot().members(0).location().shard_id(), 2);
    auto wrong = request(internal::OPERATION_SNAPSHOT);
    EXPECT_EQ(shard.handle(1, wrong).status(), internal::STATUS_NOT_FOUND);
}
}  // namespace
}  // namespace realm::game::scene
