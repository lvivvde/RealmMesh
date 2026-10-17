#pragma once

#include "realmmesh/edge/edge_connection_table.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"

#include <cstddef>
#include <cstdint>
#include <unordered_map>

namespace realm::cluster {
class InstanceBudgetReporter;
}  // namespace realm::cluster

namespace realm::edge {
class EdgeRuntime;
struct EdgeEvent;
}  // namespace realm::edge

namespace realm::observability {
class Logger;
}  // namespace realm::observability

namespace realm::game::realm {

class RealmSessions;

/// Realm 业务帧:直连入场(兑换 EnterRealm 票据)与心跳,其余已认证消息
/// 交给 RealmSessions(#93)。构造要求 REALMMESH_SESSION_TICKET_KEY 已设置
/// (缺失抛 std::runtime_error)。
class RealmFrame final {
public:
    /// conn_capacity 为接入面启用传输 max_sessions 之和,只用于上报
    /// conn_free;sessions 由调用方持有,生命周期须长于本帧。
    RealmFrame(
        std::size_t max_events_per_frame,
        std::uint64_t conn_capacity,
        RealmSessions& sessions);
    ~RealmFrame();

    void started(
        observability::Logger& logger,
        const edge::EdgeRuntime& runtime) const;
    /// drain 接入面事件并分发;budget_reporter 非空时帧尾发布 conn_free
    /// 快照,策略节流由上报器自理。
    void tick(
        observability::Logger& logger,
        edge::EdgeRuntime& runtime,
        cluster::InstanceBudgetReporter* budget_reporter = nullptr);
    void stopped(observability::Logger& logger) const;

private:
    /// 直连入场兑换(#46,ADR-0013):EnterRealm 票据单次消费,只绑定账号
    /// 与 Realm,兑换不读数据库;通过即受理并登记 Realm Session(选角阶段)。
    /// 票据无效或会话已入场回 3002 并终结(未建立会话 decline,已建立会话
    /// 回包后 close)。
    void handle_enter_realm(
        observability::Logger& logger,
        edge::EdgeRuntime& runtime,
        const edge::EdgeEvent& event,
        const common::EnterRealm& request);
    /// 连接计数与关闭簿记;返回是否为业务消息。
    [[nodiscard]] bool absorb_lifecycle(const edge::EdgeEvent& event);

    std::size_t max_events_per_frame_{0};
    common::SessionTickets tickets_;
    /// 票据 claims 以 EdgeConnectionId 寻址:入场兑换分支写入,连接关闭
    /// 时清除。
    std::unordered_map<edge::EdgeConnectionId, common::SessionTicketClaims>
        authenticated_;
    RealmSessions* sessions_;

    /// 连接额度簿记(#46):Realm 不做接入门禁(传输层已是硬上限),只按
    /// 活动连接数上报 conn_free 供放行阀门评估。
    std::uint64_t conn_capacity_{0};
    std::uint64_t conn_active_{0};
};

}  // namespace realm::game::realm
