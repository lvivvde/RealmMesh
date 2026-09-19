#pragma once

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_discovery_config.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/service_host/service_frame.hpp"

#include <atomic>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace realm::cluster {
class EtcdServiceRegistry;
class InstanceBudgetReporter;
class ServicePublisher;
class ServiceResolver;
}  // namespace realm::cluster

namespace realm::game::gateway {
class AccountFetchPort;
class GatewayLoginPipeline;
class GatewayPrimaryTransport;
class GatewayRuntime;
}  // namespace realm::game::gateway

namespace realm::game::login_verify {
class LoginVerifyService;
}  // namespace realm::game::login_verify

namespace realm::game::queue {
class QueueService;
}  // namespace realm::game::queue

namespace realm::observability {
class Logger;
class LoggerMetricsServer;
}  // namespace realm::observability

namespace realm::service_host {

/// 单服务装配:封装消息服务的公共链与业务帧循环。login_verify 与
/// queue 是 HTTPS 请求循环形态的服务(不走 ServiceFrame/EdgeSession 管线)。
/// 发现关闭时 ready() = runtime 监听成功;发现开启时还需注册 tick 成功
/// (required=true 注册失败 → start() 抛出);
/// 发现开启且服务名非 gateway/realm/login_verify/queue 时 start()
/// 抛 std::invalid_argument。
class ServiceHost final {
public:
    /// service_name/instance_id 驱动 LayeredConfigLoader;构造失败抛异常
    /// (realm/gateway 另要求 REALMMESH_SESSION_TICKET_KEY 已设置)。
    ServiceHost(
        const std::filesystem::path& config_root,
        std::string_view service_name,
        const CliOverrides& overrides = {});
    ~ServiceHost();

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    /// 启动 runtime 与发现注册;返回 ready 状态。
    [[nodiscard]] bool start();
    [[nodiscard]] bool ready() const noexcept;
    /// 优雅关停:停 runtime → 注销发现 → logger flush(2s)。
    void stop();

    [[nodiscard]] game::gateway::GatewayRuntime& runtime() noexcept;
    /// 模式 1 主循环的存活探测(形态无关):消息服务看 GatewayRuntime
    /// 是否存活,login_verify 形态没有 runtime,看 HTTPS 服务边的 running。
    [[nodiscard]] bool healthy() const noexcept;
    /// 业务体终态错误;login_verify 形态恒 nullopt(启动失败直接抛错)。
    [[nodiscard]] std::optional<std::string> terminal_error() const;
    [[nodiscard]] observability::Logger& logger() noexcept;
    /// 依赖解析(发现开启时);未开启返回 nullptr。
    [[nodiscard]] cluster::ServiceResolver* resolver() noexcept;

    /// 每帧业务帧 + 发现续约轮询(由编排器调用)。
    void tick();
    /// Prometheus 指标含 realmmesh_service_ready gauge。
    [[nodiscard]] std::string prometheus_metrics() const;

private:
    std::string service_name_;
    std::string instance_;
    cluster::ServiceDiscoveryConfig discovery_config_;
    std::atomic_bool ready_{false};
    // started_:service_started 已写(失败启动不补写 service_stopped);
    // stopped_:停机幂等标志,MeshHost::shutdown() 与析构双停只生效首次。
    bool started_{false};
    bool stopped_{false};
    // 声明序即析构序:budget_reporter → resolver → publisher → registry
    // (budget_reporter 与 publisher 持 registry 引用),
    // 再到 frame → Gateway Login Pipeline → adapters → runtime →
    // metrics → logger。
    /// 领域指标注册表(#47):首个对象成员(析构最后),业务帧与
    /// HTTPS 服务经指针写入,/metrics 抓取时渲染。
    observability::MetricsRegistry metrics_registry_;
    std::unique_ptr<observability::Logger> logger_;
    std::unique_ptr<observability::LoggerMetricsServer> metrics_;
    std::unique_ptr<game::gateway::GatewayRuntime> runtime_;
    std::unique_ptr<game::gateway::GatewayPrimaryTransport>
        gateway_primary_transport_;
    std::unique_ptr<game::gateway::AccountFetchPort> account_fetch_;
    std::unique_ptr<game::gateway::GatewayLoginPipeline>
        gateway_login_pipeline_;
    std::unique_ptr<game::login_verify::LoginVerifyService> login_verify_;
    std::unique_ptr<game::queue::QueueService> queue_;
    std::unique_ptr<ServiceFrame> frame_;
    std::unique_ptr<cluster::EtcdServiceRegistry> registry_;
    std::unique_ptr<cluster::ServicePublisher> publisher_;
    std::unique_ptr<cluster::ServiceResolver> resolver_;
    /// 额度上报器(#43,仅 gateway 且发现注册成功后装配);析构须先于
    /// registry,故声明在引用链成员之后。
    std::unique_ptr<cluster::InstanceBudgetReporter> budget_reporter_;
    /// 上报器的阈值策略:容量在构造期从网关配置同源注入(config 随后
    /// 被 move 进 runtime,tick 期不可再取)。
    cluster::BudgetPublishPolicy budget_policy_;
};

}  // namespace realm::service_host
