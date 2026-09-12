#pragma once

#include "realmmesh/game/gateway/edge_session_table.hpp"

#include <cstdint>
#include <optional>
#include <unordered_map>

namespace realm::game::gateway {

/// Edge Session 的登录管线阶段(spec §5.3;词汇见 CONTEXT.md:Edge
/// Session)。pending = 握手完成待验凭据;fetching = 凭据验讫、限额拉取;
/// handed-off = 直连凭证已下发(终态);closed = 会话终结。
enum class EdgeSessionStage : std::uint8_t {
    Pending,
    Fetching,
    HandedOff,
    Closed,
};

/// try_enter_fetching 的结果:额度外(拉取池满)时拒绝并把会话迁入
/// closed,调用方负责拒答与终结传输。
enum class EnterFetchingResult : std::uint8_t {
    Entered,
    UnknownSession,
    /// 非 pending 阶段(重复 attach、已 closed 等)。
    NotPending,
    AtCapacity,
};

/// 一次会话终结的管线侧快照:终结前的阶段(供预算归还与事件合成)。
struct ClosedPipelineSession {
    EdgeSessionId session_id;
    EdgeSessionStage stage{EdgeSessionStage::Closed};
};

/// Edge Session 登录管线阶段机 + 实例额度会计(spec #43)。
/// 与 EdgeSessionTable(传输层状态,IO 线程)并存:本表只活在业务帧
/// 线程,由 GatewayEvent 驱动登记/注销,由 attach/拉取完成驱动迁移;
/// conn 占用自会话打开起计、关闭归还,fetch 槽自进入 fetching 扣、
/// 离开 fetching(放行完成或关闭)归还。
class EdgeSessionPipeline final {
public:
    /// conn_capacity:在管会话上限(规格层总量);fetch_capacity:每实例
    /// 全局拉取并发预算池规模。
    EdgeSessionPipeline(
        std::uint64_t conn_capacity, std::uint64_t fetch_capacity);

    void on_session_opened(EdgeSessionId session_id);

    /// 会话终结(对端关闭或本地关闭):注销并返回终结前阶段;未知会话
    /// 返回 nullopt(重复终结通知安全)。
    [[nodiscard]] std::optional<ClosedPipelineSession> on_session_closed(
        EdgeSessionId session_id);

    /// pending → fetching(attach 验讫后调用);拉取池满时拒绝并迁入
    /// closed(额度外拒绝)。
    [[nodiscard]] EnterFetchingResult try_enter_fetching(
        EdgeSessionId session_id);

    /// fetching → handed-off(拉取完成、直连凭证下发;#45 接线)。
    [[nodiscard]] bool mark_handed_off(EdgeSessionId session_id);

    /// 未知会话读作 closed(未登记即不在管线内)。
    [[nodiscard]] EdgeSessionStage stage(EdgeSessionId session_id) const;

    [[nodiscard]] std::uint64_t conn_used() const noexcept { return conn_used_; }
    [[nodiscard]] std::uint64_t fetch_used() const noexcept {
        return fetch_used_;
    }
    [[nodiscard]] std::uint64_t conn_free() const noexcept {
        return conn_capacity_ - conn_used_;
    }
    [[nodiscard]] std::uint64_t fetch_free() const noexcept {
        return fetch_capacity_ - fetch_used_;
    }

private:
    struct Entry {
        EdgeSessionStage stage{EdgeSessionStage::Pending};
    };

    std::uint64_t conn_capacity_;
    std::uint64_t fetch_capacity_;
    std::uint64_t conn_used_{0};
    std::uint64_t fetch_used_{0};
    std::unordered_map<EdgeSessionId, Entry> sessions_;
};

}  // namespace realm::game::gateway
