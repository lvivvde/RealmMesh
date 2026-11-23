#include "realmmesh/game/common/scene_protocol.hpp"
#include "realmmesh/common/v1/envelope.pb.h"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/realm_protocol.hpp"
#include "realmmesh/game/common/scene_internal_protocol.hpp"
#include "realmmesh/game/common/scene_store.hpp"

#include <gtest/gtest.h>

#include <limits>
#include <string>

namespace realm::game::common {
namespace {
namespace scene = ::realmmesh::protocol::scene::v1;
namespace internal = ::realmmesh::protocol::scene_internal::v1;
using Envelope = ::realmmesh::protocol::common::v1::Envelope;

std::vector<std::byte> raw(
    const auto& message,
    std::uint32_t id,
    std::uint64_t request_id = 1,
    std::uint32_t version = 1) {
    Envelope envelope;
    envelope.set_protocol_version(version);
    envelope.set_message_id(id);
    envelope.set_request_id(request_id);
    envelope.set_payload(message.SerializeAsString());
    const auto serialized = envelope.SerializeAsString();
    const auto bytes = protobuf_bytes(serialized);
    return {bytes.begin(), bytes.end()};
}
scene::Location location() {
    scene::Location m;
    m.set_shard_id(1);
    m.set_x(10);
    m.set_y(20);
    return m;
}
scene::Member member() {
    scene::Member m;
    m.set_character_id(7);
    m.set_binding_epoch(2);
    *m.mutable_location() = location();
    return m;
}
scene::Snapshot snapshot() {
    scene::Snapshot s;
    s.set_shard_id(1);
    s.set_shard_incarnation("shard-run-1");
    s.set_event_seq(9);
    *s.add_members() = member();
    return s;
}
SceneCommandResult command_result() {
    SceneCommandResult r;
    r.set_kind(scene::COMMAND_KIND_MOVE);
    r.set_scene_seq(3);
    r.set_binding_epoch(2);
    *r.mutable_location() = location();
    return r;
}
SceneEvent event() {
    SceneEvent e;
    e.set_shard_id(1);
    e.set_shard_incarnation("shard-run-1");
    e.set_event_seq(10);
    e.set_kind(SceneEvent::KIND_MOVE);
    *e.mutable_member() = member();
    return e;
}
void identities(auto& message) {
    message.set_realm_incarnation("realm-run-1");
    message.set_scene_instance_id("scene-a");
    message.set_scene_incarnation("scene-run-1");
}
internal::Route route() {
    internal::Route r;
    r.set_realm_id(1);
    r.set_character_id(7);
    r.set_binding_epoch(2);
    r.set_session_binding(4);
    r.set_shard_id(1);
    return r;
}
SceneRequest request(internal::Operation op) {
    SceneRequest r;
    identities(r);
    r.set_operation(op);
    if (op == internal::OPERATION_STATUS) return r;
    if (op == internal::OPERATION_SET_DRAINING) {
        r.set_draining(true);
        return r;
    }
    if (op == internal::OPERATION_SNAPSHOT) {
        r.set_shard_id(1);
        return r;
    }
    *r.mutable_route() = route();
    if (op == internal::OPERATION_REMOVE) return r;
    r.set_scene_seq(3);
    r.set_command_digest(std::string(32, 'd'));
    if (op != internal::OPERATION_ACTIVATE) *r.mutable_location() = location();
    return r;
}
SceneReply reply(internal::Operation op) {
    SceneReply r;
    identities(r);
    r.set_operation(op);
    r.set_status(internal::STATUS_OK);
    if (op == internal::OPERATION_STATUS) {
        r.set_dynamic(true);
        r.set_business_ready(true);
        r.set_drain_phase(internal::DRAIN_PHASE_IDLE);
        return r;
    }
    if (op == internal::OPERATION_SET_DRAINING) {
        r.set_draining(true);
        r.set_active_members(2);
        return r;
    }
    if (op == internal::OPERATION_SNAPSHOT) {
        *r.mutable_snapshot() = snapshot();
        return r;
    }
    *r.mutable_route() = route();
    if (op == internal::OPERATION_REMOVE) return r;
    r.set_scene_seq(3);
    if (op == internal::OPERATION_ACTIVATE)
        *r.mutable_snapshot() = snapshot();
    else
        *r.mutable_location() = location();
    return r;
}

template <typename Message, typename Decode>
void roundtrip(
    const Message& message,
    Decode decode,
    std::uint32_t id,
    std::uint64_t request_id) {
    const auto bytes = encode(message, request_id);
    const auto decoded = decode(bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->SerializeAsString(), message.SerializeAsString());
    Envelope envelope;
    ASSERT_TRUE(
        envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
    EXPECT_EQ(envelope.message_id(), id);
    EXPECT_EQ(envelope.request_id(), request_id);
    EXPECT_EQ(envelope.protocol_version(), 1U);
}

TEST(
    SceneProtocolTest,
    AllClientContractsRoundTripWithoutChangingEnvelopeVersion) {
    EnterScene enter;
    enter.set_scene_seq(1);
    roundtrip(enter, decode_enter_scene, 1501, 11);
    roundtrip(command_result(), decode_scene_command_result, 1502, 12);
    MoveInScene move;
    move.set_scene_seq(3);
    move.set_x(10);
    move.set_y(20);
    roundtrip(move, decode_move_in_scene, 1503, 13);
    roundtrip(GetSceneState{}, decode_get_scene_state, 1504, 14);
    SceneState state;
    state.set_phase(scene::PHASE_ACTIVE);
    state.set_last_scene_seq(3);
    state.set_binding_epoch(2);
    *state.mutable_last_result() = command_result();
    *state.mutable_confirmed_location() = location();
    *state.mutable_snapshot() = snapshot();
    roundtrip(state, decode_scene_state, 1505, 15);
    roundtrip(event(), decode_scene_event, 1506, 0);
    TransferScene transfer;
    transfer.set_scene_seq(4);
    transfer.set_target_shard_id(2);
    roundtrip(transfer, decode_transfer_scene, 1507, 16);
    SceneUnavailable unavailable;
    unavailable.set_shard_id(1);
    unavailable.set_binding_epoch(2);
    unavailable.set_retry_after_seconds(1);
    roundtrip(unavailable, decode_scene_unavailable, 1508, 0);
    EXPECT_EQ(scene_request_id(encode(enter, 11)), 11U);
    EXPECT_EQ(scene_request_id(encode(event())), 0U);
}

TEST(SceneProtocolTest, ClientEdgeRealmAndInternalIdsCannotBeConfused) {
    const auto external = encode(GetSceneState{}, 1);
    const auto private_wire = encode(request(internal::OPERATION_SNAPSHOT), 1);
    EXPECT_FALSE(edge_message_id(external));
    EXPECT_FALSE(realm_message_id(external));
    EXPECT_FALSE(scene_internal_message_id(external));
    EXPECT_FALSE(scene_message_id(private_wire));
    EXPECT_FALSE(edge_message_id(private_wire));
    EXPECT_FALSE(realm_message_id(private_wire));
    EXPECT_FALSE(decode_get_scene_state(private_wire));
    EXPECT_FALSE(decode_scene_request(external));
    EXPECT_FALSE(scene_message_id(encode(HeartbeatRequest{}, 1)));
    EXPECT_FALSE(scene_message_id(encode(ListCharacters{}, 1)));
}

TEST(SceneProtocolTest, RejectsBadFrameVersionIdPayloadAndOversizedEnvelope) {
    EXPECT_FALSE(decode_get_scene_state(raw(GetSceneState{}, 1504, 1, 2)));
    EXPECT_FALSE(decode_get_scene_state(raw(GetSceneState{}, 1501)));
    EXPECT_FALSE(decode_enter_scene(protobuf_bytes("\xFF")));
    Envelope envelope;
    envelope.set_protocol_version(1);
    envelope.set_message_id(1501);
    envelope.set_request_id(1);
    envelope.set_payload("\xFF");
    EXPECT_FALSE(
        decode_enter_scene(protobuf_bytes(envelope.SerializeAsString())));
    const std::vector<std::byte> too_large(scene_envelope_max + 1);
    EXPECT_FALSE(scene_message_id(too_large));
    EXPECT_FALSE(decode_enter_scene(too_large));
    EXPECT_FALSE(scene_internal_message_id(too_large));
}

TEST(SceneProtocolTest, InvalidRequiredValuesAndDirectionAreRejected) {
    EXPECT_FALSE(decode_enter_scene(raw(EnterScene{}, 1501)));
    EnterScene enter;
    enter.set_scene_seq(std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(decode_enter_scene(raw(enter, 1501)));
    enter.set_scene_seq(1);
    EXPECT_THROW(static_cast<void>(encode(enter, 0)), std::invalid_argument);
    EXPECT_FALSE(decode_enter_scene(raw(enter, 1501, 0)));
    EXPECT_FALSE(decode_scene_event(raw(event(), 1506, 5)));
    MoveInScene move;
    move.set_scene_seq(1);
    move.set_x(-1);
    EXPECT_FALSE(decode_move_in_scene(raw(move, 1503)));
    move.set_x(10001);
    EXPECT_THROW(static_cast<void>(encode(move, 1)), std::invalid_argument);
    TransferScene transfer;
    transfer.set_scene_seq(1);
    EXPECT_FALSE(decode_transfer_scene(raw(transfer, 1507)));
}

TEST(SceneProtocolTest, SnapshotRejectsDuplicatesWrongShardAndOverCapacity) {
    SceneState s;
    s.set_phase(scene::PHASE_ACTIVE);
    s.set_last_scene_seq(3);
    s.set_binding_epoch(2);
    *s.mutable_last_result() = command_result();
    *s.mutable_confirmed_location() = location();
    *s.mutable_snapshot() = snapshot();
    *s.mutable_snapshot()->add_members() = member();
    EXPECT_FALSE(decode_scene_state(raw(s, 1505)));
    *s.mutable_snapshot() = snapshot();
    s.mutable_snapshot()->mutable_members(0)->mutable_location()->set_shard_id(
        2);
    EXPECT_FALSE(decode_scene_state(raw(s, 1505)));
    *s.mutable_snapshot() = snapshot();
    for (std::uint64_t i = 0; i < 16; ++i) {
        auto* m = s.mutable_snapshot()->add_members();
        *m = member();
        m->set_character_id(100 + i);
    }
    EXPECT_FALSE(decode_scene_state(raw(s, 1505)));
    s.clear_snapshot();
    EXPECT_FALSE(decode_scene_state(raw(s, 1505)));
    s.set_phase(scene::PHASE_UNAVAILABLE);
    EXPECT_TRUE(decode_scene_state(raw(s, 1505)));
    s.set_last_scene_seq(4);
    EXPECT_FALSE(decode_scene_state(raw(s, 1505)));
}

TEST(
    SceneProtocolTest,
    InternalOperationsHaveExplicitSuccessAndFailureContracts) {
    for (int operation = 1; operation <= 8; ++operation) {
        const auto op = static_cast<internal::Operation>(operation);
        roundtrip(request(op), decode_scene_request, 1601, 17);
        roundtrip(reply(op), decode_scene_reply, 1602, 17);
        for (int status = 2; status <= 7; ++status) {
            SceneReply failed;
            identities(failed);
            failed.set_operation(op);
            failed.set_status(static_cast<internal::Status>(status));
            roundtrip(failed, decode_scene_reply, 1602, 17);
        }
    }
    ScenePush push;
    identities(push);
    *push.mutable_event() = event();
    roundtrip(push, decode_scene_push, 1603, 0);
    EXPECT_EQ(
        scene_internal_request_id(
            encode(request(internal::OPERATION_REMOVE), 17)),
        17U);
}

TEST(
    SceneProtocolTest,
    InternalValidationRejectsMissingIdentityRouteDigestAndSnapshot) {
    auto r = request(internal::OPERATION_PREPARE);
    r.clear_realm_incarnation();
    EXPECT_FALSE(decode_scene_request(raw(r, 1601)));
    r = request(internal::OPERATION_PREPARE);
    r.mutable_route()->set_binding_epoch(0);
    EXPECT_FALSE(decode_scene_request(raw(r, 1601)));
    r = request(internal::OPERATION_PREPARE);
    r.set_command_digest("short");
    EXPECT_FALSE(decode_scene_request(raw(r, 1601)));
    r = request(internal::OPERATION_SNAPSHOT);
    *r.mutable_route() = route();
    EXPECT_FALSE(decode_scene_request(raw(r, 1601)));
    r = request(internal::OPERATION_REMOVE);
    EXPECT_FALSE(decode_scene_request(raw(r, 1601, 0)));
    auto response = reply(internal::OPERATION_ACTIVATE);
    response.clear_snapshot();
    EXPECT_FALSE(decode_scene_reply(raw(response, 1602)));
    response = reply(internal::OPERATION_SNAPSHOT);
    response.set_status(internal::STATUS_BUSY);
    EXPECT_FALSE(decode_scene_reply(raw(response, 1602)));
    ScenePush push;
    identities(push);
    *push.mutable_event() = event();
    EXPECT_FALSE(decode_scene_push(raw(push, 1603, 1)));
}

TEST(
    SceneProtocolTest,
    DynamicStatusRejectsFalseCompletionAndDuplicateShardStreams) {
    auto status = reply(internal::OPERATION_STATUS);
    auto* shard = status.add_shards();
    shard->set_realm_id(1);
    shard->set_shard_id(1);
    shard->set_version(2);
    shard->set_active_members(1);
    status.set_active_members(1);
    roundtrip(status, decode_scene_reply, 1602, 17);
    status.set_draining(true);
    status.set_drain_phase(internal::DRAIN_PHASE_COMPLETE);
    status.set_business_ready(false);
    EXPECT_FALSE(decode_scene_reply(raw(status, 1602)));
    status.set_drain_phase(internal::DRAIN_PHASE_RUNNING);
    *status.add_shards() = *shard;
    status.set_active_members(2);
    EXPECT_FALSE(decode_scene_reply(raw(status, 1602)));
    auto probe = request(internal::OPERATION_STATUS);
    probe.set_shard_id(1);
    EXPECT_FALSE(decode_scene_request(raw(probe, 1601)));
    auto route_request = request(internal::OPERATION_PREPARE);
    route_request.mutable_route()->set_shard_version(scene_counter_max + 1);
    EXPECT_FALSE(decode_scene_request(raw(route_request, 1601)));
}

TEST(
    SceneProtocolTest,
    UnknownOutcomeRemainsUnknownUntilExactReceiptIsObserved) {
    SceneWrite write{
        .character = {42, 1, 7},
        .expected_epoch = 1,
        .owner = {"realm-run-1", 4, "scene-a", "scene-run-1"},
        .command = {SceneCommandType::Move, 2, 10, 20},
        .location = {1, 10, 20}};
    SceneStateRecord state{.character = write.character, .binding_epoch = 1};
    EXPECT_EQ(
        resolve_scene_write(write, std::nullopt),
        SceneWriteResolution::Unknown);
    EXPECT_EQ(resolve_scene_write(write, state), SceneWriteResolution::Unknown);
    state.last_result =
        SceneStoredResult{write.command, write.location, 1, write.owner};
    EXPECT_EQ(
        resolve_scene_write(write, state), SceneWriteResolution::Committed);
    state.last_result->command.x++;
    EXPECT_EQ(resolve_scene_write(write, state), SceneWriteResolution::Unknown);
    state.last_result->command = write.command;
    state.last_result->owner.scene_incarnation += "old";
    EXPECT_EQ(resolve_scene_write(write, state), SceneWriteResolution::Unknown);
    auto command = write.command;
    const auto digest = scene_command_digest(command);
    EXPECT_EQ(digest.size(), 64U);
    command.seq++;
    EXPECT_EQ(scene_command_digest(command), digest);
    command.x++;
    EXPECT_NE(scene_command_digest(command), digest);
}

}  // namespace
}  // namespace realm::game::common
