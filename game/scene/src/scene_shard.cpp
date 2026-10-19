#include "realmmesh/game/scene/scene_shard.hpp"
#include "realmmesh/game/common/scene_store.hpp"

#include <cstdlib>
#include <utility>

namespace realm::game::scene {
namespace internal = ::realmmesh::protocol::scene_internal::v1;
namespace wire = ::realmmesh::protocol::scene::v1;

SceneShard::SceneShard(std::string instance_id, std::string incarnation)
    : instance_id_(std::move(instance_id)),
      incarnation_(std::move(incarnation)) {}

bool SceneShard::same_binding(
    const Member& member,
    std::uint64_t connection,
    const common::SceneRequest& request) {
    return member.connection == connection &&
           member.realm_incarnation == request.realm_incarnation() &&
           member.route.SerializeAsString() ==
               request.route().SerializeAsString();
}

wire::Snapshot SceneShard::snapshot() const {
    wire::Snapshot result;
    result.set_shard_id(1);
    result.set_shard_incarnation(incarnation_);
    result.set_event_seq(event_seq_);
    for (const auto& [id, member] : active_) {
        auto* value = result.add_members();
        value->set_character_id(id);
        value->set_binding_epoch(member.route.binding_epoch());
        *value->mutable_location() = member.location;
    }
    return result;
}

void SceneShard::emit(common::SceneEvent::Kind kind, const Member& member) {
    common::SceneEvent event;
    event.set_shard_id(1);
    event.set_shard_incarnation(incarnation_);
    event.set_event_seq(++event_seq_);
    event.set_kind(kind);
    auto* value = event.mutable_member();
    value->set_character_id(member.route.character_id());
    value->set_binding_epoch(member.route.binding_epoch());
    *value->mutable_location() = member.location;
    events_.push_back(std::move(event));
}

common::SceneReply SceneShard::handle(
    std::uint64_t connection,
    const common::SceneRequest& request,
    Clock::time_point now) {
    expire(now);
    common::SceneReply reply;
    reply.set_realm_incarnation(request.realm_incarnation());
    reply.set_scene_instance_id(instance_id_);
    reply.set_scene_incarnation(incarnation_);
    reply.set_operation(request.operation());
    reply.set_status(internal::STATUS_STALE_BINDING);
    const bool bootstrap =
        request.operation() == internal::OPERATION_SNAPSHOT &&
        request.scene_incarnation() == "discover";
    if (request.scene_instance_id() != instance_id_ ||
        (!bootstrap && request.scene_incarnation() != incarnation_))
        return reply;
    if (request.operation() == internal::OPERATION_SNAPSHOT) {
        if (request.shard_id() != 1) {
            reply.set_status(internal::STATUS_NOT_FOUND);
            return reply;
        }
        reply.set_status(internal::STATUS_OK);
        *reply.mutable_snapshot() = snapshot();
        return reply;
    }
    if (request.operation() == internal::OPERATION_SET_DRAINING) {
        draining_ = request.draining();
        reply.set_status(internal::STATUS_OK);
        reply.set_draining(draining_);
        reply.set_active_members(static_cast<std::uint32_t>(active_.size()));
        return reply;
    }
    if (request.route().realm_id() != 1 || request.route().shard_id() != 1) {
        reply.set_status(internal::STATUS_NOT_FOUND);
        return reply;
    }
    const auto id = request.route().character_id();
    const auto active = active_.find(id);
    const auto prepared = prepared_.find(id);
    const auto candidate = candidates_.find(id);
    const auto ok_route = [&] {
        reply.set_status(internal::STATUS_OK);
        *reply.mutable_route() = request.route();
    };
    const auto ok_position = [&] {
        ok_route();
        reply.set_scene_seq(request.scene_seq());
        *reply.mutable_location() = request.location();
    };
    const auto receipt_matches = [&](const Member& member) {
        return same_binding(member, connection, request) &&
               member.seq == request.scene_seq() &&
               member.digest == request.command_digest();
    };
    switch (request.operation()) {
    case internal::OPERATION_PREPARE: {
        if (active != active_.end() && receipt_matches(active->second) &&
            active->second.location.SerializeAsString() ==
                request.location().SerializeAsString()) {
            ok_position();
            return reply;
        }
        if (prepared != prepared_.end() && receipt_matches(prepared->second) &&
            prepared->second.location.SerializeAsString() ==
                request.location().SerializeAsString()) {
            ok_position();
            return reply;
        }
        if ((active != active_.end() &&
             !(same_binding(active->second, connection, request) &&
               request.scene_seq() > active->second.seq) &&
             request.route().binding_epoch() <=
                 active->second.route.binding_epoch()) ||
            (prepared != prepared_.end() &&
             request.route().binding_epoch() <=
                 prepared->second.route.binding_epoch()))
            return reply;
        if (draining_) {
            reply.set_status(internal::STATUS_DRAINING);
            return reply;
        }
        std::size_t occupied = active_.size();
        for (const auto& [key, member] : prepared_) {
            static_cast<void>(member);
            if (!active_.contains(key)) ++occupied;
        }
        if (occupied >= 16 && !active_.contains(id) &&
            !prepared_.contains(id)) {
            reply.set_status(internal::STATUS_BUSY);
            return reply;
        }
        prepared_[id] = Member{
            connection,
            request.realm_incarnation(),
            request.route(),
            request.scene_seq(),
            request.command_digest(),
            request.location(),
            now + std::chrono::seconds(3)};
        ok_position();
        return reply;
    }
    case internal::OPERATION_ACTIVATE: {
        if (active != active_.end() && receipt_matches(active->second)) {
            ok_route();
            reply.set_scene_seq(request.scene_seq());
            *reply.mutable_snapshot() = snapshot();
            return reply;
        }
        if (prepared == prepared_.end() || !receipt_matches(prepared->second))
            return reply;
        const bool same = active != active_.end() &&
                          same_binding(active->second, connection, request);
        if (active != active_.end() && !same)
            emit(common::SceneEvent::KIND_LEAVE, active->second);
        active_[id] = std::move(prepared->second);
        prepared_.erase(prepared);
        candidates_.erase(id);
        emit(
            same ? common::SceneEvent::KIND_MOVE
                 : common::SceneEvent::KIND_JOIN,
            active_.at(id));
        ok_route();
        reply.set_scene_seq(request.scene_seq());
        *reply.mutable_snapshot() = snapshot();
        return reply;
    }
    case internal::OPERATION_PROPOSE_MOVE: {
        if (active == active_.end() ||
            !same_binding(active->second, connection, request))
            return reply;
        const auto& member = active->second;
        if (request.scene_seq() != member.seq + 1) {
            reply.set_status(internal::STATUS_INVALID);
            return reply;
        }
        const auto distance =
            std::abs(request.location().x() - member.location.x()) +
            std::abs(request.location().y() - member.location.y());
        if (distance > 100 || request.location().x() < 0 ||
            request.location().x() > 10000 || request.location().y() < 0 ||
            request.location().y() > 10000) {
            reply.set_status(internal::STATUS_INVALID);
            return reply;
        }
        if (candidate != candidates_.end() &&
            (!receipt_matches(candidate->second) ||
             candidate->second.location.SerializeAsString() !=
                 request.location().SerializeAsString())) {
            reply.set_status(internal::STATUS_BUSY);
            return reply;
        }
        candidates_[id] = Member{
            connection,
            request.realm_incarnation(),
            request.route(),
            request.scene_seq(),
            request.command_digest(),
            request.location(),
            now + std::chrono::seconds(3)};
        ok_position();
        return reply;
    }
    case internal::OPERATION_CONFIRM: {
        if (active != active_.end() && receipt_matches(active->second) &&
            active->second.location.SerializeAsString() ==
                request.location().SerializeAsString()) {
            ok_position();
            return reply;
        }
        if (candidate == candidates_.end() ||
            !receipt_matches(candidate->second) ||
            candidate->second.location.SerializeAsString() !=
                request.location().SerializeAsString())
            return reply;
        active_[id] = std::move(candidate->second);
        candidates_.erase(candidate);
        emit(common::SceneEvent::KIND_MOVE, active_.at(id));
        ok_position();
        return reply;
    }
    case internal::OPERATION_REMOVE: {
        if (active != active_.end() &&
            same_binding(active->second, connection, request)) {
            emit(common::SceneEvent::KIND_LEAVE, active->second);
            active_.erase(active);
        }
        if (prepared != prepared_.end() &&
            same_binding(prepared->second, connection, request))
            prepared_.erase(prepared);
        if (candidate != candidates_.end() &&
            same_binding(candidate->second, connection, request))
            candidates_.erase(candidate);
        ok_route();
        return reply;
    }
    default:
        reply.set_status(internal::STATUS_INVALID);
        return reply;
    }
}

void SceneShard::closed(std::uint64_t connection) {
    for (auto it = active_.begin(); it != active_.end();) {
        if (it->second.connection == connection) {
            emit(common::SceneEvent::KIND_LEAVE, it->second);
            candidates_.erase(it->first);
            it = active_.erase(it);
        } else
            ++it;
    }
    std::erase_if(prepared_, [connection](const auto& item) {
        return item.second.connection == connection;
    });
    std::erase_if(candidates_, [connection](const auto& item) {
        return item.second.connection == connection;
    });
}
void SceneShard::expire(Clock::time_point now) {
    std::erase_if(prepared_, [now](const auto& item) {
        return item.second.deadline <= now;
    });
    std::erase_if(candidates_, [now](const auto& item) {
        return item.second.deadline <= now;
    });
}
std::vector<common::SceneEvent> SceneShard::drain_events() {
    return std::exchange(events_, {});
}
}  // namespace realm::game::scene
