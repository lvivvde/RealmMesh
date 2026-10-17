#pragma once

// Scene 两条线共享的值校验；不向消费者传播存储或驱动类型。
#include "envelope_codec.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"
#include "realmmesh/game/common/scene_store.hpp"

#include <algorithm>
#include <stdexcept>
#include <string_view>
#include <unordered_set>

namespace realm::game::common::detail {
namespace scene_v1 = ::realmmesh::protocol::scene::v1;
inline bool scene_counter(std::uint64_t value) {
    return value > 0 && value <= scene_counter_max;
}
inline bool scene_identity_text(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](unsigned char c) {
               return c >= 33 && c <= 126;
           });
}
inline bool scene_xy(std::int32_t x, std::int32_t y) {
    return x >= 0 && x <= scene_coordinate_max && y >= 0 &&
           y <= scene_coordinate_max;
}
inline bool valid_scene_location(const scene_v1::Location& location) {
    return location.shard_id() != 0 && scene_xy(location.x(), location.y());
}
inline bool same_scene_location(
    const scene_v1::Location& a, const scene_v1::Location& b) {
    return a.shard_id() == b.shard_id() && a.x() == b.x() && a.y() == b.y();
}
inline bool valid_scene_member(const scene_v1::Member& member) {
    return member.character_id() != 0 &&
           scene_counter(member.binding_epoch()) && member.has_location() &&
           valid_scene_location(member.location());
}
inline bool valid_scene_snapshot(const scene_v1::Snapshot& snapshot) {
    if (snapshot.shard_id() == 0 ||
        !scene_identity_text(snapshot.shard_incarnation()) ||
        snapshot.event_seq() > scene_counter_max ||
        snapshot.members_size() > static_cast<int>(scene_snapshot_members_max))
        return false;
    std::unordered_set<std::uint64_t> members;
    for (const auto& member : snapshot.members()) {
        if (!valid_scene_member(member) ||
            member.location().shard_id() != snapshot.shard_id() ||
            !members.insert(member.character_id()).second)
            return false;
    }
    return true;
}
inline bool valid_scene_result(const SceneCommandResult& result) {
    return result.kind() >= scene_v1::COMMAND_KIND_ENTER &&
           result.kind() <= scene_v1::COMMAND_KIND_TRANSFER &&
           scene_counter(result.scene_seq()) &&
           scene_counter(result.binding_epoch()) && result.has_location() &&
           valid_scene_location(result.location());
}
inline bool valid_scene_event(const SceneEvent& event) {
    return event.shard_id() != 0 &&
           scene_identity_text(event.shard_incarnation()) &&
           scene_counter(event.event_seq()) &&
           event.kind() >= SceneEvent::KIND_JOIN &&
           event.kind() <= SceneEvent::KIND_LEAVE && event.has_member() &&
           valid_scene_member(event.member()) &&
           event.member().location().shard_id() == event.shard_id();
}

template <typename Message, typename Validator>
std::vector<std::byte> encode_scene_message(
    const Message& message,
    std::uint32_t id,
    std::uint64_t request_id,
    Validator validator) {
    if (message.ByteSizeLong() > scene_envelope_max ||
        !validator(message, request_id))
        throw std::invalid_argument("invalid scene message");
    auto payload = encode_message(message, id, request_id);
    if (payload.size() > scene_envelope_max)
        throw std::invalid_argument("scene envelope exceeds 64KiB");
    return payload;
}

template <typename Message, typename Validator>
std::optional<Message> decode_scene_message(
    std::span<const std::byte> payload, std::uint32_t id, Validator validator) {
    if (payload.size() > scene_envelope_max) return std::nullopt;
    const auto envelope = parse_envelope(payload);
    if (!envelope || envelope->message_id() != id) return std::nullopt;
    auto message = decode_message<Message>(payload, id);
    if (!message || !validator(*message, envelope->request_id()))
        return std::nullopt;
    return message;
}
}  // namespace realm::game::common::detail
