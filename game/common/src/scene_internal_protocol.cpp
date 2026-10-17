#include "realmmesh/game/common/scene_internal_protocol.hpp"
#include "scene_protocol_validation.hpp"

namespace realm::game::common {
namespace {
namespace internal = ::realmmesh::protocol::scene_internal::v1;

bool valid_route(const internal::Route& route) {
    return route.realm_id() != 0 && route.character_id() != 0 &&
           detail::scene_counter(route.binding_epoch()) &&
           route.session_binding() != 0 && route.shard_id() != 0;
}
template <typename Message>
bool valid_identity(const Message& message) {
    return detail::scene_identity_text(message.realm_incarnation()) &&
           detail::scene_identity_text(message.scene_instance_id()) &&
           detail::scene_identity_text(message.scene_incarnation());
}
bool valid_operation(internal::Operation op) {
    return op >= internal::OPERATION_PREPARE &&
           op <= internal::OPERATION_SET_DRAINING;
}
bool valid(const SceneRequest& m, std::uint64_t id) {
    if (id == 0 || !valid_identity(m) || !valid_operation(m.operation()))
        return false;
    if (m.operation() == internal::OPERATION_SET_DRAINING) {
        return !m.has_route() && !m.has_location() && m.scene_seq() == 0 &&
               m.shard_id() == 0 && m.command_digest().empty();
    }
    if (m.draining()) return false;
    if (m.operation() == internal::OPERATION_SNAPSHOT) {
        return m.shard_id() != 0 && !m.has_route() && !m.has_location() &&
               m.scene_seq() == 0 && m.command_digest().empty();
    }
    if (m.shard_id() != 0 || !m.has_route() || !valid_route(m.route()))
        return false;
    if (m.operation() == internal::OPERATION_REMOVE) {
        return m.scene_seq() == 0 && m.command_digest().empty() &&
               !m.has_location();
    }
    if (!detail::scene_counter(m.scene_seq()) ||
        m.command_digest().size() != 32)
        return false;
    if (m.operation() == internal::OPERATION_ACTIVATE) return !m.has_location();
    return m.has_location() && detail::valid_scene_location(m.location()) &&
           m.route().shard_id() == m.location().shard_id();
}
bool valid(const SceneReply& m, std::uint64_t id) {
    if (id == 0 || !valid_identity(m) || !valid_operation(m.operation()) ||
        m.status() < internal::STATUS_OK ||
        m.status() > internal::STATUS_UNAVAILABLE)
        return false;
    // 拒绝回复不携带快照、候选位置或不完整路由；关联 ID 和操作足够定位原请求。
    if (m.status() != internal::STATUS_OK)
        return !m.has_route() && !m.has_location() && !m.has_snapshot() &&
               m.scene_seq() == 0 && m.active_members() == 0 && !m.draining();
    if (m.operation() == internal::OPERATION_SET_DRAINING) {
        return !m.has_route() && !m.has_location() && !m.has_snapshot() &&
               m.scene_seq() == 0;
    }
    if (m.draining() || m.active_members() != 0) return false;
    if (m.operation() == internal::OPERATION_SNAPSHOT) {
        return !m.has_route() && !m.has_location() && m.scene_seq() == 0 &&
               m.has_snapshot() && detail::valid_scene_snapshot(m.snapshot());
    }
    if (!m.has_route() || !valid_route(m.route())) return false;
    if (m.operation() == internal::OPERATION_REMOVE)
        return m.scene_seq() == 0 && !m.has_location() && !m.has_snapshot();
    if (!detail::scene_counter(m.scene_seq())) return false;
    if (m.operation() == internal::OPERATION_ACTIVATE) {
        return !m.has_location() && m.has_snapshot() &&
               detail::valid_scene_snapshot(m.snapshot()) &&
               m.snapshot().shard_id() == m.route().shard_id();
    }
    return !m.has_snapshot() && m.has_location() &&
           detail::valid_scene_location(m.location()) &&
           m.location().shard_id() == m.route().shard_id();
}
bool valid(const ScenePush& m, std::uint64_t id) {
    return id == 0 && valid_identity(m) && m.has_event() &&
           detail::valid_scene_event(m.event());
}
}  // namespace

std::optional<SceneInternalMessageId> scene_internal_message_id(
    std::span<const std::byte> payload) {
    if (payload.size() > scene_envelope_max) return std::nullopt;
    const auto envelope = detail::parse_envelope(payload);
    if (!envelope || envelope->message_id() < 1601 ||
        envelope->message_id() > 1603)
        return std::nullopt;
    return static_cast<SceneInternalMessageId>(envelope->message_id());
}
std::optional<std::uint64_t> scene_internal_request_id(
    std::span<const std::byte> payload) {
    if (!scene_internal_message_id(payload)) return std::nullopt;
    return detail::parse_envelope(payload)->request_id();
}
std::vector<std::byte> encode(
    const SceneRequest& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message, 1601, request_id, [](const SceneRequest& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<SceneRequest> decode_scene_request(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<SceneRequest>(
        payload, 1601, [](const SceneRequest& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const SceneReply& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message, 1602, request_id, [](const SceneReply& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<SceneReply> decode_scene_reply(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<SceneReply>(
        payload, 1602, [](const SceneReply& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const ScenePush& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message, 1603, request_id, [](const ScenePush& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<ScenePush> decode_scene_push(std::span<const std::byte> payload) {
    return detail::decode_scene_message<ScenePush>(
        payload, 1603, [](const ScenePush& m, std::uint64_t id) {
            return valid(m, id);
        });
}
}  // namespace realm::game::common
