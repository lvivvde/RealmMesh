#pragma once

namespace realm::cluster {
class InstanceBudgetReporter;
class ServiceResolver;
}  // namespace realm::cluster

namespace realm::edge {
class EdgeRuntime;
}  // namespace realm::edge

namespace realm::observability {
class Logger;
}  // namespace realm::observability

namespace realm::game::gateway {

class GatewayLoginPipeline;

/// Gateway 业务帧:完整登录管线由服务模块在监听前构造,本帧每帧只提供
/// 时间与当前 Realm 端点并调用一次 advance(),再据结果更新就绪、发布
/// 额度快照;管线依赖停止时停掉接入面运行时。
class GatewayFrame final {
public:
    /// pipeline 由调用方持有,生命周期须长于本帧。
    explicit GatewayFrame(GatewayLoginPipeline& pipeline) noexcept;

    /// 每个启用传输一条 listener_started,再发 service_started。
    void started(
        observability::Logger& logger,
        const edge::EdgeRuntime& runtime) const;
    /// resolver 非空且有端点时以发现到的 Realm 端点为准,否则管线退回
    /// 静态下游;budget_reporter 非空时帧尾发布双维度额度快照。
    void tick(
        observability::Logger& logger,
        edge::EdgeRuntime& runtime,
        cluster::ServiceResolver* resolver,
        cluster::InstanceBudgetReporter* budget_reporter = nullptr);
    /// service_stopped,附接入面运行时统计。
    void stopped(
        observability::Logger& logger,
        const edge::EdgeRuntime& runtime) const;
    /// 管线健康且本地额度可用。
    [[nodiscard]] bool ready() const noexcept;

private:
    GatewayLoginPipeline* pipeline_;
    bool ready_{true};
};

}  // namespace realm::game::gateway
