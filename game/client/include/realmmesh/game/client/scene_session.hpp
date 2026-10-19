#pragma once
#include <optional>
#include <variant>
#include "realmmesh/game/client/login_chain.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"

namespace realm::game::client {
using SceneNotice = std::variant<common::SceneEvent, common::SceneUnavailable>;
/// 复用 Login Chain 已建立的 Realm Session,一次只执行一个调用。
/// Enter 后 GetSceneState 取得快照,随后 poll_scene 消费事件;Unavailable 后
/// 再取状态恢复快照。请求等待期间的推送有界缓存,不会被心跳/训练吞掉。
class SceneSession {
public:
    virtual ~SceneSession() = default;
    [[nodiscard]] virtual RealmReply<common::SceneCommandResult> enter_scene(
        std::uint64_t scene_seq, TimePoint deadline) = 0;
    [[nodiscard]] virtual RealmReply<common::SceneCommandResult> move_in_scene(
        std::uint64_t scene_seq,
        std::int32_t x,
        std::int32_t y,
        TimePoint deadline) = 0;
    [[nodiscard]] virtual RealmReply<common::SceneState> get_scene_state(
        TimePoint deadline) = 0;
    [[nodiscard]] virtual std::optional<SceneNotice> poll_scene(
        TimePoint deadline) = 0;
};
}  // namespace realm::game::client
