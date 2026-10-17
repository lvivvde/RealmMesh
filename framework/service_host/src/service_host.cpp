#include "realmmesh/service_host/service_host.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/cluster/service_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace realm::service_host {
namespace {

/// 注册到发现中心的服务版本(与原 main 装配一致)。
constexpr std::string_view service_version = "0.1.0";

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
    const ServiceModuleRegistry& modules,
    const CliOverrides& overrides)
    : service_name_(service_name) {
    // 纯配置校验先行:未注册服务名在装载配置或触碰 etcd 前即失败。
    const ServiceModuleRegistration* registration = modules.find(service_name);
    if (registration == nullptr) {
        throw std::invalid_argument(
            "unsupported service name: " + service_name_);
    }
    dependency_ = registration->dependency;
    const LayeredConfig config =
        LayeredConfigLoader::load(config_root, service_name, overrides);
    const HostConfig& host = config.host();
    discovery_config_ = host.service_discovery;
    // 实例标识默认值与 LayeredConfigLoader 的日志文件命名保持一致。
    instance_ = discovery_config_.instance_id.empty()
                    ? service_name_ + "-01"
                    : discovery_config_.instance_id;
    logger_ = std::make_unique<observability::Logger>(
        host.logging, host.logging_identity);
    if (host.logging_metrics.port != 0) {
        metrics_ = std::make_unique<observability::LoggerMetricsServer>(
            [this] {
                return prometheus_metrics();
            },
            host.logging_metrics);
    }
    module_ = registration->factory(ServiceModuleContext{
        .config = config,
        .instance_id = instance_,
        .logger = *logger_,
        .metrics = metrics_registry_,
    });
    if (module_ == nullptr) {
        throw std::runtime_error(
            "service module factory returned null: " + service_name_);
    }
}

ServiceHost::~ServiceHost() { stop(); }

bool ServiceHost::start() {
    module_->start();
    if (!module_->running()) return false;
    try {
        if (discovery_config_.enabled) {
            // 重启场景按引用链反序清理旧装配,避免 publisher 悬挂 registry。
            budget_reporter_.reset();
            resolver_.reset();
            publisher_.reset();
            registry_.reset();
            registry_ = std::make_unique<cluster::EtcdServiceRegistry>(
                cluster::make_etcd_registry_options(discovery_config_));
            publisher_ = std::make_unique<cluster::ServicePublisher>(
                *registry_,
                cluster::make_service_instance(
                    service_name_,
                    discovery_config_,
                    module_->local_endpoints(),
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
            if (dependency_.has_value()) {
                resolver_ = std::make_unique<cluster::ServiceResolver>(
                    *registry_,
                    *dependency_,
                    network::TransportProtocol::TlsTcp);
            }
        }
        module_->started();
        // 重启场景:成功启动后复位停机标志,允许再次 stop() 写出事件。
        started_ = true;
        stopped_ = false;
        const bool discovery_ready =
            !discovery_config_.enabled ||
            (publisher_ != nullptr && publisher_->registered());
        ready_.store(discovery_ready && module_->ready());
        return ready_.load();
    } catch (...) {
        module_->stop();
        budget_reporter_.reset();
        resolver_.reset();
        publisher_.reset();
        registry_.reset();
        ready_.store(false);
        throw;
    }
}

bool ServiceHost::ready() const noexcept { return ready_.load(); }

void ServiceHost::stop() {
    // 幂等:MeshHost::shutdown() 与 ServiceHost 析构双停只生效首次;
    // 未写过 service_started 的失败启动不补写无配对的 service_stopped。
    if (stopped_) return;
    stopped_ = true;
    if (module_ != nullptr) {
        module_->stop();
        if (started_) module_->stopped();
    }
    // 注销发现:budget_reporter/publisher/resolver 持有 registry 引用,
    // 须先行析构。
    budget_reporter_.reset();
    resolver_.reset();
    publisher_.reset();
    registry_.reset();
    ready_.store(false);
    if (logger_ != nullptr) {
        static_cast<void>(logger_->flush(std::chrono::seconds(2)));
    }
}

ServiceModule& ServiceHost::module() noexcept { return *module_; }

bool ServiceHost::healthy() const noexcept { return module_->running(); }

std::optional<std::string> ServiceHost::terminal_error() const {
    return module_->terminal_error();
}

observability::Logger& ServiceHost::logger() noexcept { return *logger_; }

cluster::ServiceResolver* ServiceHost::resolver() noexcept {
    return resolver_.get();
}

void ServiceHost::tick() {
    module_->tick(ServiceTickContext{
        .resolver = resolver_.get(),
        .budget_reporter = budget_reporter_.get(),
    });
    const bool running = module_->running();
    if (!running) {
        ready_.store(false);
    }
    if (publisher_ != nullptr && publisher_->tick()) {
        // 注册成功后装配额度上报器(#43 gateway / #46 realm):required=false
        // 时首注册可能失败,注册成功后的首个 tick 补齐。上报资格由模块的
        // 额度策略声明。
        if (budget_reporter_ == nullptr && publisher_->registered()) {
            if (auto policy = module_->budget_policy(); policy.has_value()) {
                budget_reporter_ =
                    std::make_unique<cluster::InstanceBudgetReporter>(
                        *registry_,
                        publisher_->registration_id(),
                        service_name_,
                        instance_,
                        *policy);
                budget_reporter_->set_failure_sink(
                    [this](const std::string& key) {
                        static_cast<void>(logger_->warn(
                            "instance_budget_publish_failed",
                            "instance budget publish failed; will retry when "
                            "policy allows",
                            {observability::field("key", key)}));
                    });
            }
        }
    }
    const bool discovery_ready =
        !discovery_config_.enabled ||
        (publisher_ != nullptr && publisher_->registered());
    ready_.store(running && discovery_ready && module_->ready());
}

std::string ServiceHost::prometheus_metrics() const {
    std::string output = logger_->prometheus_metrics();
    // 领域指标(#47):日志管道指标之后、service_ready 之前,拼接顺序
    // 既有约定不变。
    output += metrics_registry_.render();
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
