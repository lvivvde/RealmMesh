#pragma once
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include "realmmesh/edge/edge_connection_table.hpp"
#include "realmmesh/game/common/scene_store.hpp"

namespace realm::network {
struct TransportEndpoint;
}
namespace realm::observability {
class MetricsRegistry;
}
namespace realm::game::realm {
class RealmOutbox;
struct RealmConfig;

/// 场景专用执行器:网络与 MongoDB 只在工作线程访问,帧线程只入队/收结果。
/// 与角色数据工作池隔离;关闭立即使绑定失效,清理由工作线程按旧版本执行。
class RealmScenes final {
public:
    RealmScenes(
        common::RealmSceneStore& store,
        RealmOutbox& outbox,
        const RealmConfig& config,
        observability::MetricsRegistry& metrics);
    ~RealmScenes();
    RealmScenes(const RealmScenes&) = delete;
    RealmScenes& operator=(const RealmScenes&) = delete;
    void selected(
        edge::EdgeConnectionId id, common::SceneCharacterKey character);
    void closed(edge::EdgeConnectionId id);
    [[nodiscard]] bool handle(
        edge::EdgeConnectionId id, std::span<const std::byte> payload);
    void complete(std::size_t max_completions);
    void endpoint(const std::optional<network::TransportEndpoint>& value);

private:
    // RealmSessions 的现有消费者不需要带入 TLS、线程队列和内部协议布局。
    class Worker;
    std::unique_ptr<Worker> worker_;
};
}  // namespace realm::game::realm
