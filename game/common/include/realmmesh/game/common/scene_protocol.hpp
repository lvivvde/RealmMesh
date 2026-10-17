#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "realmmesh/scene/v1/scene.pb.h"

namespace realm::game::common {
inline constexpr int scene_error_not_active = 3101;
inline constexpr int scene_error_invalid_target = 3102;
inline constexpr int scene_error_invalid_seq = 3103;
inline constexpr int scene_error_unavailable = 3104;
inline constexpr int scene_error_outcome_unknown = 3105;
inline constexpr int scene_error_stale_binding = 3106;
inline constexpr std::size_t scene_envelope_max = 64 * 1024;
inline constexpr std::size_t scene_snapshot_members_max = 16;
using SceneMessageId = ::realmmesh::protocol::scene::v1::MessageId;
using EnterScene = ::realmmesh::protocol::scene::v1::EnterScene;
using SceneCommandResult = ::realmmesh::protocol::scene::v1::SceneCommandResult;
using MoveInScene = ::realmmesh::protocol::scene::v1::MoveInScene;
using GetSceneState = ::realmmesh::protocol::scene::v1::GetSceneState;
using SceneState = ::realmmesh::protocol::scene::v1::SceneState;
using SceneEvent = ::realmmesh::protocol::scene::v1::SceneEvent;
using TransferScene = ::realmmesh::protocol::scene::v1::TransferScene;
using SceneUnavailable = ::realmmesh::protocol::scene::v1::SceneUnavailable;
[[nodiscard]] std::optional<SceneMessageId> scene_message_id(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<std::uint64_t> scene_request_id(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const EnterScene& message, std::uint64_t request_id);
[[nodiscard]] std::optional<EnterScene> decode_enter_scene(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const SceneCommandResult& message, std::uint64_t request_id);
[[nodiscard]] std::optional<SceneCommandResult> decode_scene_command_result(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const MoveInScene& message, std::uint64_t request_id);
[[nodiscard]] std::optional<MoveInScene> decode_move_in_scene(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const GetSceneState& message, std::uint64_t request_id);
[[nodiscard]] std::optional<GetSceneState> decode_get_scene_state(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const SceneState& message, std::uint64_t request_id);
[[nodiscard]] std::optional<SceneState> decode_scene_state(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const SceneEvent& message, std::uint64_t request_id = 0);
[[nodiscard]] std::optional<SceneEvent> decode_scene_event(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const TransferScene& message, std::uint64_t request_id);
[[nodiscard]] std::optional<TransferScene> decode_transfer_scene(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const SceneUnavailable& message, std::uint64_t request_id = 0);
[[nodiscard]] std::optional<SceneUnavailable> decode_scene_unavailable(
    std::span<const std::byte> payload);
}  // namespace realm::game::common
