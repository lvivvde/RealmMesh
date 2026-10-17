#pragma once

// 服务模块契约(ADR-0014):ServiceHost 只认识这一层接口——宿主负责配置
// 装载、日志、指标端口、发现注册与额度上报,业务体由服务模块提供。服务名
// 到模块工厂的映射由进程入口(apps/)装配进 ServiceModuleRegistry,framework
// 不点名任何游戏服务。
#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/network/transport/message_transport.hpp"
#include "realmmesh/service_host/layered_config.hpp"

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace realm::cluster {
class InstanceBudgetReporter;
class ServiceResolver;
}  // namespace realm::cluster

namespace realm::observability {
class Logger;
class MetricsRegistry;
}  // namespace realm::observability

namespace realm::service_host {

/// 模块构造输入:引用只在工厂调用期间有效,模块须自行拷贝所需配置值;
/// logger 与 metrics 由宿主持有,生命周期长于模块。
struct ServiceModuleContext {
    const LayeredConfig& config;
    std::string_view instance_id;
    observability::Logger& logger;
    observability::MetricsRegistry& metrics;
};

/// 每帧由宿主提供的集群句柄;发现关闭或尚未就绪时为 nullptr。
struct ServiceTickContext {
    /// 注册项声明了依赖服务时的端点解析。
    cluster::ServiceResolver* resolver{nullptr};
    /// 模块声明了额度策略且注册成功后才装配。
    cluster::InstanceBudgetReporter* budget_reporter{nullptr};
};

/// 单个服务的业务体。宿主调用次序:start → (发现注册)→ started →
/// tick* → stop → stopped(仅 started 之后)。
class ServiceModule {
public:
    virtual ~ServiceModule() = default;

    /// 绑定监听;失败抛异常或让 running() 为 false。
    virtual void start() = 0;
    /// 停止监听与工作线程;须幂等。
    virtual void stop() = 0;
    [[nodiscard]] virtual bool running() const noexcept = 0;
    /// 发现注册用的本地端点(start 之后有效)。
    [[nodiscard]] virtual std::span<const network::TransportEndpoint>
    local_endpoints() const noexcept = 0;
    /// 每帧业务。
    virtual void tick(const ServiceTickContext& context) = 0;

    /// 启动完成(含发现注册)后的生命周期事件,如 service_started。
    virtual void started() {}
    /// 停止后的生命周期事件,如 service_stopped。
    virtual void stopped() {}
    /// 业务就绪;宿主再与发现注册状态合取。
    [[nodiscard]] virtual bool ready() const noexcept { return true; }
    /// 业务体终态错误(模式 1 主循环据此退出)。
    [[nodiscard]] virtual std::optional<std::string> terminal_error() const {
        return std::nullopt;
    }
    /// 额度上报策略;有值时宿主在注册成功后装配 InstanceBudgetReporter。
    [[nodiscard]] virtual std::optional<cluster::BudgetPublishPolicy>
    budget_policy() const {
        return std::nullopt;
    }
};

using ServiceModuleFactory =
    std::function<std::unique_ptr<ServiceModule>(const ServiceModuleContext&)>;

struct ServiceModuleRegistration {
    ServiceModuleFactory factory;
    /// 发现开启时解析的依赖服务类型(取其 TLS/TCP 端点)。
    std::optional<std::string> dependency;
    /// 模式 2 单服务拓扑:启动前等待依赖服务可连(发现或静态监听)。
    bool mode2_waits_for_dependency{false};
};

/// 服务名 → 模块注册项。服务名即集群服务类型,须满足
/// cluster::is_valid_service_type。
class ServiceModuleRegistry final {
public:
    /// 名字非法或重复注册抛 std::invalid_argument。
    void add(std::string name, ServiceModuleRegistration registration);
    /// 未注册返回 nullptr。
    [[nodiscard]] const ServiceModuleRegistration* find(
        std::string_view name) const noexcept;

private:
    std::map<std::string, ServiceModuleRegistration, std::less<>> modules_;
};

}  // namespace realm::service_host
