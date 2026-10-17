#include "realmmesh/game/common/scene_protocol.hpp"
#include "scene_protocol_validation.hpp"

namespace realm::game::common {
namespace {
namespace scene_v1 = ::realmmesh::protocol::scene::v1;
bool valid(const EnterScene& m, std::uint64_t id) {
    return id != 0 && detail::scene_counter(m.scene_seq());
}
bool valid(const MoveInScene& m, std::uint64_t id) {
    return id != 0 && detail::scene_counter(m.scene_seq()) &&
           detail::scene_xy(m.x(), m.y());
}
bool valid(const TransferScene& m, std::uint64_t id) {
    return id != 0 && detail::scene_counter(m.scene_seq()) &&
           m.target_shard_id() != 0;
}
bool valid(const GetSceneState&, std::uint64_t id) { return id != 0; }
bool valid(const SceneCommandResult& m, std::uint64_t id) {
    return id != 0 && detail::valid_scene_result(m);
}
bool valid(const SceneEvent& m, std::uint64_t id) {
    return id == 0 && detail::valid_scene_event(m);
}
bool valid(const SceneUnavailable& m, std::uint64_t id) {
    return id == 0 && m.shard_id() != 0 &&
           detail::scene_counter(m.binding_epoch()) &&
           m.retry_after_seconds() > 0 && m.retry_after_seconds() <= 5;
}
bool valid(const SceneState& m, std::uint64_t id) {
    if (id == 0 || m.phase() < scene_v1::PHASE_OUTSIDE ||
        m.phase() > scene_v1::PHASE_UNAVAILABLE ||
        m.binding_epoch() > scene_counter_max ||
        m.last_scene_seq() > scene_counter_max)
        return false;
    if (m.last_scene_seq() == 0) {
        if (m.has_last_result() || m.has_confirmed_location() ||
            m.binding_epoch() != 0)
            return false;
    } else {
        if (!m.has_last_result() ||
            !detail::valid_scene_result(m.last_result()) ||
            m.last_result().scene_seq() != m.last_scene_seq() ||
            m.last_result().binding_epoch() > m.binding_epoch() ||
            !m.has_confirmed_location() ||
            !detail::same_scene_location(
                m.confirmed_location(), m.last_result().location()))
            return false;
    }
    if (m.has_snapshot() &&
        (!detail::valid_scene_snapshot(m.snapshot()) ||
         !m.has_confirmed_location() ||
         m.snapshot().shard_id() != m.confirmed_location().shard_id()))
        return false;
    return m.phase() != scene_v1::PHASE_ACTIVE ||
           (m.has_snapshot() && m.last_scene_seq() != 0);
}
}  // namespace

std::optional<SceneMessageId> scene_message_id(
    std::span<const std::byte> payload) {
    if (payload.size() > scene_envelope_max) return std::nullopt;
    const auto envelope = detail::parse_envelope(payload);
    if (!envelope || envelope->message_id() < 1501 ||
        envelope->message_id() > 1508)
        return std::nullopt;
    return static_cast<SceneMessageId>(envelope->message_id());
}
std::optional<std::uint64_t> scene_request_id(
    std::span<const std::byte> payload) {
    if (!scene_message_id(payload)) return std::nullopt;
    return detail::parse_envelope(payload)->request_id();
}
std::vector<std::byte> encode(
    const EnterScene& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message, 1501, request_id, [](const EnterScene& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<EnterScene> decode_enter_scene(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<EnterScene>(
        payload, 1501, [](const EnterScene& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const SceneCommandResult& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message,
        1502,
        request_id,
        [](const SceneCommandResult& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<SceneCommandResult> decode_scene_command_result(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<SceneCommandResult>(
        payload, 1502, [](const SceneCommandResult& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const MoveInScene& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message, 1503, request_id, [](const MoveInScene& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<MoveInScene> decode_move_in_scene(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<MoveInScene>(
        payload, 1503, [](const MoveInScene& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const GetSceneState& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message,
        1504,
        request_id,
        [](const GetSceneState& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<GetSceneState> decode_get_scene_state(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<GetSceneState>(
        payload, 1504, [](const GetSceneState& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const SceneState& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message, 1505, request_id, [](const SceneState& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<SceneState> decode_scene_state(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<SceneState>(
        payload, 1505, [](const SceneState& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const SceneEvent& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message, 1506, request_id, [](const SceneEvent& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<SceneEvent> decode_scene_event(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<SceneEvent>(
        payload, 1506, [](const SceneEvent& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const TransferScene& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message,
        1507,
        request_id,
        [](const TransferScene& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<TransferScene> decode_transfer_scene(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<TransferScene>(
        payload, 1507, [](const TransferScene& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::vector<std::byte> encode(
    const SceneUnavailable& message, std::uint64_t request_id) {
    return detail::encode_scene_message(
        message,
        1508,
        request_id,
        [](const SceneUnavailable& m, std::uint64_t id) {
            return valid(m, id);
        });
}
std::optional<SceneUnavailable> decode_scene_unavailable(
    std::span<const std::byte> payload) {
    return detail::decode_scene_message<SceneUnavailable>(
        payload, 1508, [](const SceneUnavailable& m, std::uint64_t id) {
            return valid(m, id);
        });
}
}  // namespace realm::game::common
