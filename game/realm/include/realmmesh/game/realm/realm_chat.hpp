#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include "realmmesh/edge/edge_connection_table.hpp"
namespace realm::observability {
class MetricsRegistry;
}
namespace realm::game::realm {
class RealmOutbox;
class RealmScenes;
struct RealmConfig;
/// 帧线程只维护在线资格、提交有界操作和取完成；TLS 只在每承载的 IO 线程执行。
/// 与 RealmScenes 同样隐藏内部 protobuf/线程布局，避免传播到 RealmSessions
/// 消费者。
class RealmChat final {
public:
    RealmChat(
        RealmOutbox&,
        const RealmConfig&,
        observability::MetricsRegistry&,
        RealmScenes* = nullptr);
    ~RealmChat();
    RealmChat(const RealmChat&) = delete;
    RealmChat& operator=(const RealmChat&) = delete;
    void selected(edge::EdgeConnectionId, std::uint64_t character_id);
    void closed(edge::EdgeConnectionId);
    bool handle(edge::EdgeConnectionId, std::span<const std::byte>);
    void complete(std::size_t max_completions);

private:
    class State;
    std::unique_ptr<State> state_;
};
}  // namespace realm::game::realm
