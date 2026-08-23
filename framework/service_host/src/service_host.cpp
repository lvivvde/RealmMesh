#include "realmmesh/service_host/service_host.hpp"

#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/cluster/service_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <stdexcept>
#include <string>
#include <utility>

namespace realm::service_host {
namespace {

/// 注册到发现中心的服务版本(与原 main 装配一致)。
constexpr std::string_view service_version = "0.1.0";

/// 注册自身类型映射:gateway/realm/login 之外的名字无发现语义。
[[nodiscard]] cluster::ServiceType self_service_type(
    std::string_view service_name) {
    if (service_name == "gateway") return cluster::ServiceType::Gateway;
    if (service_name == "realm") return cluster::ServiceType::Realm;
    if (service_name == "login") return cluster::ServiceType::Login;
    throw std::invalid_argument(
        "unsupported service name: " + std::string(service_name));
}

/// 依赖解析对象映射:gateway→Login、login→Realm、realm→Gateway。
[[nodiscard]] cluster::ServiceType dependency_service_type(
    std::string_view service_name) {
    if (service_name == "gateway") return cluster::ServiceType::Login;
    if (service_name == "login") return cluster::ServiceType::Realm;
    if (service_name == "realm") return cluster::ServiceType::Gateway;
    throw std::invalid_argument(
        "unsupported service name: " + std::string(service_name));
}

/// 转义 Prometheus 标签值中的 \ " 与换行,保证指标输出格式合法
/// (与 observability 内部 prometheus_label 逻辑一致,该助手未导出)。
[[nodiscard]] std::string prometheus_label(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

}  // namespace

ServiceHost::ServiceHost(
    const std::filesystem::path& config_root,
    std::string_view service_name,
    const CliOverrides& overrides)
    : service_name_(service_name) {
    auto config =
        LayeredConfigLoader::load(config_root, service_name, overrides);
    discovery_config_ = config.service_discovery;
    // 实例标识默认值与 LayeredConfigLoader 的日志文件命名保持一致。
    instance_ = discovery_config_.instance_id.empty()
                    ? service_name_ + "-01"
                    : discovery_config_.instance_id;
    logger_ = std::make_unique<observability::Logger>(
        config.logging, config.logging_identity);
    if (config.logging_metrics.port != 0) {
        metrics_ = std::make_unique<observability::LoggerMetricsServer>(
            *logger_, config.logging_metrics);
    }
    frame_ = std::make_unique<ServiceFrame>(
        service_name_,
        config.downstream_address,
        config.downstream_port,
        config.max_events_per_frame);
    runtime_ = std::make_unique<game::gateway::GatewayRuntime>(
        std::move(config), logger_.get());
}

ServiceHost::~ServiceHost() { stop(); }

bool ServiceHost::start() {
    if (discovery_config_.enabled) {
        // 名字映射先行:未知服务名在触碰 etcd 前即失败。
        const auto self_type = self_service_type(service_name_);
        const auto dependency_type = dependency_service_type(service_name_);
        // 重启场景按引用链反序清理旧装配,避免 publisher 悬挂 registry。
        resolver_.reset();
        publisher_.reset();
        registry_.reset();
        registry_ = std::make_unique<cluster::EtcdServiceRegistry>(
            cluster::make_etcd_registry_options(discovery_config_));
        publisher_ = std::make_unique<cluster::ServicePublisher>(
            *registry_,
            cluster::make_service_instance(
                self_type,
                discovery_config_,
                runtime_->local_endpoints(),
                service_version),
            discovery_config_.lease_ttl);
        const bool registered = publisher_->tick();
        if (!registered && discovery_config_.required) {
            throw std::runtime_error(
                service_name_ +
                " service registration failed: " + registry_->last_error());
        }
        if (!registered) {
            static_cast<void>(logger_->warn(
                "dependency_state_changed",
                "service discovery unavailable; using Lua fallback",
                {observability::field("dependency", "etcd"),
                 observability::field("state", "unavailable"),
                 observability::field(
                     "error_message", registry_->last_error())}));
        }
        resolver_ = std::make_unique<cluster::ServiceResolver>(
            *registry_, dependency_type, network::TransportProtocol::TlsTcp);
    }
    runtime_->start();
    if (!runtime_->running()) return false;
    frame_->started(*logger_, *runtime_);
    // 重启场景:成功启动后复位停机标志,允许再次 stop() 写出事件。
    started_ = true;
    stopped_ = false;
    if (!discovery_config_.enabled ||
        (publisher_ != nullptr && publisher_->registered())) {
        ready_.store(true);
    }
    return ready_.load();
}

bool ServiceHost::ready() const noexcept { return ready_.load(); }

void ServiceHost::stop() {
    // 幂等:MeshHost::shutdown() 与 ServiceHost 析构双停只生效首次;
    // 未写过 service_started 的失败启动不补写无配对的 service_stopped。
    if (stopped_) return;
    stopped_ = true;
    if (runtime_ != nullptr) runtime_->stop();
    if (started_ && runtime_ != nullptr && logger_ != nullptr &&
        frame_ != nullptr) {
        frame_->stopped(*logger_, *runtime_);
    }
    // 注销发现:publisher/resolver 持有 registry 引用,须先行析构。
    resolver_.reset();
    publisher_.reset();
    registry_.reset();
    ready_.store(false);
    if (logger_ != nullptr) {
        static_cast<void>(logger_->flush(std::chrono::seconds(2)));
    }
}

game::gateway::GatewayRuntime& ServiceHost::runtime() noexcept {
    return *runtime_;
}

observability::Logger& ServiceHost::logger() noexcept { return *logger_; }

cluster::ServiceResolver* ServiceHost::resolver() noexcept {
    return resolver_.get();
}

void ServiceHost::tick() {
    if (frame_ != nullptr && runtime_ != nullptr) {
        frame_->tick(*logger_, *runtime_, resolver_.get());
    }
    if (publisher_ == nullptr) return;
    if (!publisher_->tick()) return;
    // required=false 时首注册可能失败,续约成功后补齐 ready。
    if (runtime_ != nullptr && runtime_->running()) {
        ready_.store(true);
    }
}

std::string ServiceHost::prometheus_metrics() const {
    std::string output = logger_->prometheus_metrics();
    output += "# TYPE realmmesh_service_ready gauge\n";
    output += "realmmesh_service_ready{service_name=\"";
    output += prometheus_label(service_name_);
    output += "\",service_instance=\"";
    output += prometheus_label(instance_);
    output += "\"} ";
    output += ready_.load() ? "1" : "0";
    output += '\n';
    return output;
}

}  // namespace realm::service_host
