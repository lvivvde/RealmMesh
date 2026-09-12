#pragma once

#include "realmmesh/cluster/service_discovery_config.hpp"
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
class ServicePublisher;
class ServiceResolver;
}  // namespace realm::cluster

namespace realm::game::gateway {
class GatewayRuntime;
}  // namespace realm::game::gateway

namespace realm::game::login_verify {
class LoginVerifyService;
}  // namespace realm::game::login_verify

namespace realm::observability {
class Logger;
class LoggerMetricsServer;
}  // namespace realm::observability

namespace realm::service_host {

/// 单服务装配:封装原三个 main 的公共链与业务帧循环。login_verify 是
/// 第二种服务形态(HTTPS 请求循环,不走 ServiceFrame/EdgeSession 管线)。
/// 发现关闭时 ready() = runtime 监听成功;发现开启时还需注册 tick 成功
/// (required=true 注册失败 → start() 抛出);
/// 发现开启且服务名非 gateway/realm/login/login_verify 时 start() 抛
/// std::invalid_argument。
class ServiceHost final {
public:
    /// service_name/instance_id 驱动 LayeredConfigLoader;构造失败抛异常
    /// (login/realm/gateway 另要求 REALMMESH_SESSION_TICKET_KEY 已设置)。
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
    // 声明序即析构序:resolver → publisher → registry(引用链),
    // 再到 frame → login_verify/runtime → metrics → logger。
    std::unique_ptr<observability::Logger> logger_;
    std::unique_ptr<observability::LoggerMetricsServer> metrics_;
    std::unique_ptr<game::gateway::GatewayRuntime> runtime_;
    std::unique_ptr<game::login_verify::LoginVerifyService> login_verify_;
    std::unique_ptr<ServiceFrame> frame_;
    std::unique_ptr<cluster::EtcdServiceRegistry> registry_;
    std::unique_ptr<cluster::ServicePublisher> publisher_;
    std::unique_ptr<cluster::ServiceResolver> resolver_;
};

}  // namespace realm::service_host
