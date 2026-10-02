#pragma once

#include "realmmesh/cluster/service_registry.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/edge_session_table.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace realm::cluster {
class ServiceResolver;
class InstanceBudgetReporter;
}  // namespace realm::cluster

namespace realm::game::gateway {
class GatewayRuntime;
struct GatewayEvent;
class GatewayLoginPipeline;
}  // namespace realm::game::gateway

namespace realm::game::common {
class PlayerDataReader;
}  // namespace realm::game::common

namespace realm::observability {
class Logger;
}  // namespace realm::observability

namespace realm::service_host {

/// 服务名 → 集群身份的唯一映射;gateway/realm/login_verify/queue 之外
/// 的名字无身份。
[[nodiscard]] std::optional<cluster::ServiceType> parse_service_identity(
    std::string_view service_name);

/// Realm 入场角色复核的工作线程池规模。容量统计排队、运行中与已完成
/// 未取走的复核;满额时新的 1304 回 429(带 retry_after_seconds),不消费票据。
struct RealmCharacterCheckLimits final {
    std::size_t workers{4};
    std::size_t capacity{256};
};

/// 单服务业务帧:承载 Realm 与 Gateway 的边协议消息循环。构造时把服务名
/// 解析为身份;无身份的服务名不处理业务消息(帧循环空转)。
/// Realm 要求 REALMMESH_SESSION_TICKET_KEY 已设置(缺失时构造抛
/// std::runtime_error),且必须注入 PlayerDataReader(缺失时构造抛
/// std::invalid_argument);入场角色复核在有界工作线程上执行,不占帧线程。Gateway 的完整登录管线由宿主在监听前构造，
/// ServiceFrame 每帧只提供时间、当前 Realm 端点并调用一次 advance()。
class ServiceFrame final {
public:
    /// downstream 为 Realm 现有配置契约所需的静态下游。
    /// conn_capacity 仅用于 Realm 的本地连接额度；Gateway 额度由其
    /// GatewayLoginPipeline 返回。
    ServiceFrame(
        std::string_view service_name,
        std::string downstream_address,
        std::uint16_t downstream_port,
        std::size_t max_events_per_frame,
        std::uint64_t conn_capacity,
        game::gateway::GatewayLoginPipeline* gateway_login_pipeline = nullptr,
        game::common::PlayerDataReader* player_data = nullptr,
        RealmCharacterCheckLimits character_checks = {});
    ~ServiceFrame();

    /// service_started(gateway 另发每 transport 的 listener_started)。
    void started(
        observability::Logger& logger,
        const game::gateway::GatewayRuntime& runtime) const;
    /// 每帧业务:drain 事件并按服务名分发处理。budget_reporter 非空时
    /// 帧尾发布额度快照(gateway 双维度、realm 仅 conn_free),策略节流
    /// 由上报器自理。
    void tick(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver,
        cluster::InstanceBudgetReporter* budget_reporter = nullptr);
    /// service_stopped(gateway 带 runtime 统计)。
    void stopped(
        observability::Logger& logger,
        const game::gateway::GatewayRuntime& runtime) const;
    /// Gateway readiness includes the admission backend's cached health;
    /// non-gateway frames are ready once their runtime is listening.
    [[nodiscard]] bool ready() const noexcept;

private:
    /// 会话生命周期簿记:SessionClosed 清除票据 claims,SessionEstablished
    /// 无携带状态(claims 在入场兑换分支写入);返回是否为业务消息。
    [[nodiscard]] bool absorb_lifecycle(
        const game::gateway::GatewayEvent& event);
    void handle_realm_events(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::InstanceBudgetReporter* budget_reporter);
    /// 直连入场兑换(#46):EnterRealm 票据单次消费,验签通过后把角色
    /// 复核交给工作线程;复核额度满时回 429 且不烧票。票据或复核失败回
    /// 3002 并终结(未建立会话 decline,已建立会话回包后 close)。
    void handle_enter_realm(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        const game::gateway::GatewayEvent& event,
        const game::common::EnterRealm& request);
    /// 取回已完成的角色复核并受理或拒绝对应会话。
    void complete_character_checks(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime);
    void handle_gateway_events(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver,
        cluster::InstanceBudgetReporter* budget_reporter);

    std::string service_name_;
    std::size_t max_events_per_frame_{0};
    std::optional<cluster::ServiceType> identity_;
    game::common::SessionTickets tickets_;
    /// 票据 claims 以 EdgeSessionId 寻址:Realm 的入场兑换分支先写入,
    /// accept 成功(SessionEstablished)后生效,SessionClosed 时清除。
    std::unordered_map<
        game::gateway::EdgeSessionId,
        game::common::SessionTicketClaims>
        authenticated_;

    /// Gateway 登录的唯一业务缝；状态、命令重试、票据、预算和指标均
    /// 由该实例拥有。ServiceFrame 不拥有其生命周期。
    game::gateway::GatewayLoginPipeline* gateway_login_pipeline_{nullptr};

    /// Realm 入场时重新核对票据角色仍属于账号与当前 Realm;Realm 身份
    /// 下非空(构造期校验),其余身份不使用。
    game::common::PlayerDataReader* player_data_{nullptr};
    /// 仅 Realm 身份构造:角色复核工作线程池与待决复核簿记。析构先于
    /// player_data_ 的属主(ServiceHost)回收工作线程。
    struct CharacterChecks;
    std::unique_ptr<CharacterChecks> character_checks_;

    /// realm 连接额度簿记(#46):conn_capacity 为启用传输 max_sessions
    /// 之和(与 gateway 同源注入);realm 不做接入门禁(传输层已是硬
    /// 上限),只按活动连接数上报 conn_free 供放行阀门评估。
    std::uint64_t conn_capacity_{0};
    std::uint64_t realm_conn_active_{0};
    bool gateway_ready_{true};
};

}  // namespace realm::service_host
