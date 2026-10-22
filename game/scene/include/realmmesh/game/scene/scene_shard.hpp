#pragma once

#include "realmmesh/game/common/scene_internal_protocol.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace realm::game::scene {

/// 单分片状态机。只产生候选;CONFIRM/ACTIVATE 表示 Realm 已持久提交。
/// 所有方法由 Scene 帧线程串行调用,时间可注入以验证资源回收。
class SceneShard final {
public:
    using Clock = std::chrono::steady_clock;
    SceneShard(
        std::string instance_id,
        std::string incarnation,
        std::uint64_t shard_id = 1,
        std::uint64_t shard_version = 0);
    [[nodiscard]] common::SceneReply handle(
        std::uint64_t connection,
        const common::SceneRequest& request,
        Clock::time_point now = Clock::now());
    void closed(std::uint64_t connection);
    void expire(Clock::time_point now = Clock::now());
    [[nodiscard]] std::vector<common::SceneEvent> drain_events();
    [[nodiscard]] std::size_t active_members() const noexcept {
        return active_.size();
    }
    [[nodiscard]] std::size_t prepared_members() const noexcept {
        return prepared_.size();
    }
    [[nodiscard]] bool draining() const noexcept { return draining_; }
    void begin_draining() noexcept { draining_ = true; }
    [[nodiscard]] const std::string& incarnation() const noexcept {
        return incarnation_;
    }

private:
    struct Member {
        std::uint64_t connection{0};
        std::string realm_incarnation;
        ::realmmesh::protocol::scene_internal::v1::Route route;
        std::uint64_t seq{0};
        std::string digest;
        ::realmmesh::protocol::scene::v1::Location location;
        Clock::time_point deadline;
    };
    [[nodiscard]] static bool same_binding(
        const Member& member,
        std::uint64_t connection,
        const common::SceneRequest& request);
    [[nodiscard]] ::realmmesh::protocol::scene::v1::Snapshot snapshot() const;
    void emit(common::SceneEvent::Kind kind, const Member& member);
    std::string instance_id_;
    std::string incarnation_;
    std::uint64_t shard_id_;
    std::uint64_t shard_version_;
    std::string event_incarnation_;
    bool draining_{false};
    std::uint64_t event_seq_{0};
    std::map<std::uint64_t, Member> active_;
    std::map<std::uint64_t, Member> prepared_;
    std::map<std::uint64_t, Member> candidates_;
    std::vector<common::SceneEvent> events_;
};
}  // namespace realm::game::scene
