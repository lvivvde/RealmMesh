#pragma once

#include "realmmesh/cluster/service_discovery_config.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/service_host/service_module.hpp"

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

namespace realm::observability {
class Logger;
class LoggerMetricsServer;
}  // namespace realm::observability

namespace realm::service_host {

/// 单服务宿主(ADR-0014):按服务名在 ServiceModuleRegistry 里取模块工厂,
/// 自持分层配置装载、日志、指标端口、发现注册与额度上报,业务体全部交给
/// ServiceModule。发现关闭时 ready() = 模块运行且就绪;发现开启时还需注册
/// tick 成功(required=true 注册失败 → start() 抛出)。
class ServiceHost final {
public:
    /// 服务名未注册 → std::invalid_argument(先于配置装载);配置或模块
    /// 构造失败抛异常。
    ServiceHost(
        const std::filesystem::path& config_root,
        std::string_view service_name,
        const ServiceModuleRegistry& modules,
        const CliOverrides& overrides = {});
    ~ServiceHost();

    ServiceHost(const ServiceHost&) = delete;
    ServiceHost& operator=(const ServiceHost&) = delete;

    /// 启动模块与发现注册;返回 ready 状态。
    [[nodiscard]] bool start();
    [[nodiscard]] bool ready() const noexcept;
    /// 优雅关停:停模块 → 注销发现 → logger flush(2s)。
    void stop();

    [[nodiscard]] ServiceModule& module() noexcept;
    /// 模式 1 主循环的存活探测:模块是否仍在运行。
    [[nodiscard]] bool healthy() const noexcept;
    /// 模块终态错误。
    [[nodiscard]] std::optional<std::string> terminal_error() const;
    [[nodiscard]] observability::Logger& logger() noexcept;
    /// 依赖解析(发现开启且注册项声明依赖时);否则返回 nullptr。
    [[nodiscard]] cluster::ServiceResolver* resolver() noexcept;

    /// 每帧模块业务 + 发现续约轮询(由编排器调用)。
    void tick();
    /// Prometheus 指标含 realmmesh_service_ready gauge。
    [[nodiscard]] std::string prometheus_metrics() const;

private:
    std::string service_name_;
    std::string instance_;
    std::optional<std::string> dependency_;
    cluster::ServiceDiscoveryConfig discovery_config_;
    std::atomic_bool ready_{false};
    // started_:service_started 已写(失败启动不补写 service_stopped);
    // stopped_:停机幂等标志,MeshHost::shutdown() 与析构双停只生效首次。
    bool started_{false};
    bool stopped_{false};
    // 声明序即析构序的反序:budget_reporter → resolver → publisher →
    // registry(budget_reporter 与 publisher 持 registry 引用)→ module →
    // metrics → logger → 领域指标注册表。
    /// 领域指标注册表(#47):首个对象成员(析构最后),模块经引用写入,
    /// /metrics 抓取时渲染。
    observability::MetricsRegistry metrics_registry_;
    std::unique_ptr<observability::Logger> logger_;
    std::unique_ptr<observability::LoggerMetricsServer> metrics_;
    std::unique_ptr<ServiceModule> module_;
    std::unique_ptr<cluster::EtcdServiceRegistry> registry_;
    std::unique_ptr<cluster::ServicePublisher> publisher_;
    std::unique_ptr<cluster::ServiceResolver> resolver_;
    /// 额度上报器(模块声明额度策略且发现注册成功后装配);析构须先于
    /// registry,故声明在引用链成员之后。
    std::unique_ptr<cluster::InstanceBudgetReporter> budget_reporter_;
};

}  // namespace realm::service_host
