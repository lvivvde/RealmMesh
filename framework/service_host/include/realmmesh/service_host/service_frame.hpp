#pragma once

#include "realmmesh/cluster/service_registry.hpp"
#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/edge_attach_chain.hpp"
#include "realmmesh/game/gateway/edge_fetch_scheduler.hpp"
#include "realmmesh/game/gateway/edge_session_pipeline.hpp"
#include "realmmesh/game/gateway/edge_session_table.hpp"

#include <chrono>
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
class EdgeFetchSource;
}  // namespace realm::game::gateway

namespace realm::observability {
class Logger;
class MetricsRegistry;
}  // namespace observability

namespace realm::service_host {

/// 服务名 → 集群身份的唯一映射;gateway/realm/login 之外的名字无身份。
[[nodiscard]] std::optional<cluster::ServiceType> parse_service_identity(
    std::string_view service_name);

/// Edge 登录管线额度上限(#43):conn=在管会话上限(规格层总量),
/// 由启用传输的 max_sessions 之和同源注入(管线满即传输满);
/// fetch=每实例拉取并发预算池规模。仅 gateway 身份消费,其余服务忽略。
struct EdgePipelineCaps {
    std::uint64_t conn_capacity{10'000};
    std::uint64_t fetch_capacity{1'000};
};

/// Edge 登录管线时序参数(#44/#45):retry_* 语义与不变量见
/// EdgeFetchScheduler;handoff_grace 为签发 EnterRealm 票据后保留
/// 会话的宽限,到期未迁移由帧头关闭。
struct EdgePipelineTuning {
    std::chrono::milliseconds retry_base{2'000};
    unsigned retry_max{3};
    std::chrono::milliseconds handoff_grace{5'000};
};

/// 单服务业务帧:搬运自旧 apps/{login,realm,gateway}/main.cpp 的消息循环。
/// 构造时把服务名解析为身份;无身份的服务名不处理业务消息(帧循环空转)。
/// 已知服务要求 REALMMESH_SESSION_TICKET_KEY 已设置(与旧 main 一致,
/// 缺失时构造抛 std::runtime_error)。
/// gateway 身份额外持有 Edge 登录管线(阶段机 + 额度会计,#43):管线
/// 只活在业务帧线程,由 GatewayEvent 驱动登记/注销,由 attach 驱动迁移。
class ServiceFrame final {
public:
    /// downstream 为静态兜底下游(login→realm、realm→gateway;gateway 不用)。
    /// edge_fetch_source(#44):拉取源由宿主注入;空则用内部延迟桩
    /// (恒成功、100ms 自报耗时,真 DB 接入前的过渡形态)。
    ServiceFrame(
        std::string_view service_name,
        std::string downstream_address,
        std::uint16_t downstream_port,
        std::size_t max_events_per_frame,
        EdgePipelineCaps edge_pipeline_caps = {},
        EdgePipelineTuning edge_pipeline_tuning = {},
        game::gateway::EdgeFetchSource* edge_fetch_source = nullptr,
        observability::MetricsRegistry* metrics = nullptr);
    /// 成员含前置声明的调度器/源,析构收敛到 cpp(完整类型可见处)。
    ~ServiceFrame();

    /// service_started(gateway 另发每 transport 的 listener_started)。
    void started(
        observability::Logger& logger,
        const game::gateway::GatewayRuntime& runtime) const;
    /// 每帧业务:drain 事件并按服务名分发处理。budget_reporter 非空时
    /// (gateway 身份)帧尾发布管线额度快照,策略节流由上报器自理。
    void tick(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver,
        cluster::InstanceBudgetReporter* budget_reporter = nullptr);
    /// service_stopped(gateway 带 runtime 统计)。
    void stopped(
        observability::Logger& logger,
        const game::gateway::GatewayRuntime& runtime) const;

private:
    /// 会话生命周期簿记:SessionClosed 清除票据 claims,SessionEstablished
    /// 无携带状态(claims 在 authenticate 分支写入);返回是否为业务消息。
    [[nodiscard]] bool absorb_lifecycle(
        const game::gateway::GatewayEvent& event);
    void handle_login_events(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver);
    void handle_realm_events(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver);
    void handle_gateway_events(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver,
        cluster::InstanceBudgetReporter* budget_reporter);
    /// handoff 签发(#45):Realm 发现端点优先、静态下游兜底,签发
    /// EnterRealm 票据推送 1303 并进入宽限;两者皆缺为降级,告警并断开。
    void grant_handoff(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver,
        game::gateway::EdgeSessionId session_id,
        std::uint64_t account_id);
    /// attach 校验链分发(#43):校验链裁决 → 按阶段回包;拒绝一律终结
    /// 会话(状态机 Pending→Closed:凭据无效/额度外拒绝)。
    void handle_edge_attach(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        const game::gateway::GatewayEvent& event,
        const game::common::EdgeAttach& attach);

    /// 帧尾指标发布(#47):从既有缝轮询读管线阶段/双预算/调度器重试/
    /// 链重放计数并写入注册表(域内核零污染);registry 为空时是空操作。
    void publish_edge_metrics();

    std::string service_name_;
    std::string downstream_address_;
    std::uint16_t downstream_port_{0};
    std::size_t max_events_per_frame_{0};
    std::optional<cluster::ServiceType> identity_;
    game::common::SessionTickets tickets_;
    /// 票据 claims 以 EdgeSessionId 寻址:authenticate 分支先写入,
    /// accept 成功(SessionEstablished)后生效,SessionClosed 时清除。
    std::unordered_map<
        game::gateway::EdgeSessionId,
        game::common::SessionTicketClaims>
        authenticated_;

    /// Edge 登录管线(仅 gateway):与 EdgeSessionTable 并存的业务帧
    /// 线程镜像,conn 占用自会话打开起计、关闭归还。
    std::optional<game::gateway::EdgeSessionPipeline> pipeline_;

    /// 限流拉取(仅 gateway,#44):默认源由帧自持、外部源归宿主;
    /// 声明顺序即析构逆序 —— 调度器引用源,必须先于源析构。
    std::unique_ptr<game::gateway::EdgeFetchSource> default_fetch_source_;
    std::optional<game::gateway::EdgeFetchScheduler> fetch_scheduler_;

    /// handoff 宽限簿记(#45):已签发票据的会话在宽限到期后由帧头
    /// 关闭;SessionClosed 主动清除。
    std::unordered_map<
        game::gateway::EdgeSessionId,
        std::chrono::steady_clock::time_point>
        handoff_deadlines_;
    std::chrono::milliseconds handoff_grace_{5'000};

    /// attach 验签上下文:codec 依赖环境注入的签名种子,首条 attach 时
    /// 惰性构造(缺种子的既有部署/测试不受影响);构造失败置
    /// attach_unavailable_ 只告警一次,后续 attach 一律按凭据无效拒绝。
    struct EdgeAttachContext {
        game::common::IdentityTokenCodec identity_codec;
        game::common::QueueNumberCodec number_codec;
        game::gateway::EdgeAttachChain chain;

        EdgeAttachContext(
            game::common::Ed25519Seed identity_seed,
            game::common::Ed25519Seed number_seed,
            game::gateway::EdgeSessionPipeline& pipeline,
            std::string_view identity_kid,
            std::string_view number_kid,
            std::string_view identity_issuer);
    };
    std::optional<EdgeAttachContext> attach_;
    bool attach_unavailable_{false};

    /// Prometheus 指标注册表(#47):可空(测试/无暴露场景),帧尾
    /// 轮询发布 edge_* 指标。
    observability::MetricsRegistry* metrics_{nullptr};
};

}  // namespace realm::service_host
