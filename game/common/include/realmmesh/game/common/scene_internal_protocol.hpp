#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>
#include "realmmesh/scene_internal/v1/scene_internal.pb.h"
namespace realm::game::common {
inline constexpr const char* scene_internal_alpn = "realmmesh-scene/1";
using SceneInternalMessageId =
    ::realmmesh::protocol::scene_internal::v1::MessageId;
using SceneRequest = ::realmmesh::protocol::scene_internal::v1::SceneRequest;
using SceneReply = ::realmmesh::protocol::scene_internal::v1::SceneReply;
using ScenePush = ::realmmesh::protocol::scene_internal::v1::ScenePush;
[[nodiscard]] std::optional<SceneInternalMessageId> scene_internal_message_id(
    std::span<const std::byte> payload);
[[nodiscard]] std::optional<std::uint64_t> scene_internal_request_id(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const SceneRequest& message, std::uint64_t request_id);
[[nodiscard]] std::optional<SceneRequest> decode_scene_request(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const SceneReply& message, std::uint64_t request_id);
[[nodiscard]] std::optional<SceneReply> decode_scene_reply(
    std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(
    const ScenePush& message, std::uint64_t request_id = 0);
[[nodiscard]] std::optional<ScenePush> decode_scene_push(
    std::span<const std::byte> payload);
}  // namespace realm::game::common
