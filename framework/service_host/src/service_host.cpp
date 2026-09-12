#include "realmmesh/service_host/service_host.hpp"

#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/cluster/service_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/game/login_verify/login_verify_service.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/service_host/service_frame.hpp"

#include <chrono>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

namespace realm::service_host {
namespace {

/// 注册到发现中心的服务版本(与原 main 装配一致)。
constexpr std::string_view service_version = "0.1.0";

/// 依赖解析对象映射:gateway→Login、login→Realm、realm→Gateway;
/// login_verify 无下游依赖(无状态 HTTPS 服务)。
[[nodiscard]] std::optional<cluster::ServiceType> dependency_service_type(
    std::string_view service_name) {
    if (service_name == "gateway") return cluster::ServiceType::Login;
    if (service_name == "login") return cluster::ServiceType::Realm;
    if (service_name == "realm") return cluster::ServiceType::Gateway;
    if (service_name == "login_verify") return std::nullopt;
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
    // login_verify 是第二种服务形态:宿主级节段仍走 GatewayConfig 的
    // 解析面,业务体换成 LoginVerifyService(无 ServiceFrame/EdgeSession)。
    std::unique_ptr<game::login_verify::LoginVerifyService> login_verify;
    game::gateway::GatewayConfig config;
    if (service_name_ == "login_verify") {
        auto login_verify_config = LayeredConfigLoader::load_login_verify(
            config_root, service_name, overrides);
        login_verify =
            std::make_unique<game::login_verify::LoginVerifyService>(
                std::move(login_verify_config.login_verify));
        config = std::move(login_verify_config.host);
    } else {
        config =
            LayeredConfigLoader::load(config_root, service_name, overrides);
    }
    discovery_config_ = config.service_discovery;
    // 实例标识默认值与 LayeredConfigLoader 的日志文件命名保持一致。
    instance_ = discovery_config_.instance_id.empty()
                    ? service_name_ + "-01"
                    : discovery_config_.instance_id;
    logger_ = std::make_unique<observability::Logger>(
        config.logging, config.logging_identity);
    if (config.logging_metrics.port != 0) {
        metrics_ = std::make_unique<observability::LoggerMetricsServer>(
            [this] {
                return prometheus_metrics();
            },
            config.logging_metrics);
    }
    if (login_verify != nullptr) {
        login_verify_ = std::move(login_verify);
        return;
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
    std::optional<cluster::ServiceType> self_type;
    std::optional<cluster::ServiceType> dependency_type;
    if (discovery_config_.enabled) {
        // 纯配置校验先行:未知服务名在启动 I/O 或触碰 etcd 前即失败。
        self_type = parse_service_identity(service_name_);
        if (!self_type.has_value()) {
            throw std::invalid_argument(
                "unsupported service name: " + service_name_);
        }
        dependency_type = dependency_service_type(service_name_);
    }
    bool running = false;
    if (login_verify_ != nullptr) {
        login_verify_->start(logger_.get());
        running = login_verify_->running();
    } else {
        runtime_->start();
        running = runtime_->running();
    }
    if (!running) return false;
    try {
        if (discovery_config_.enabled) {
            // 重启场景按引用链反序清理旧装配,避免 publisher 悬挂 registry。
            resolver_.reset();
            publisher_.reset();
            registry_.reset();
            registry_ = std::make_unique<cluster::EtcdServiceRegistry>(
                cluster::make_etcd_registry_options(discovery_config_));
            publisher_ = std::make_unique<cluster::ServicePublisher>(
                *registry_,
                cluster::make_service_instance(
                    *self_type,
                    discovery_config_,
                    login_verify_ != nullptr
                        ? std::span<const network::TransportEndpoint>(
                              login_verify_->local_endpoints())
                        : runtime_->local_endpoints(),
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
            if (dependency_type.has_value()) {
                resolver_ = std::make_unique<cluster::ServiceResolver>(
                    *registry_,
                    *dependency_type,
                    network::TransportProtocol::TlsTcp);
            }
        }
        if (frame_ != nullptr) {
            frame_->started(*logger_, *runtime_);
        }
        // 重启场景:成功启动后复位停机标志,允许再次 stop() 写出事件。
        started_ = true;
        stopped_ = false;
        if (!discovery_config_.enabled ||
            (publisher_ != nullptr && publisher_->registered())) {
            ready_.store(true);
        }
        return ready_.load();
    } catch (...) {
        if (login_verify_ != nullptr) {
            login_verify_->stop();
        } else if (runtime_ != nullptr) {
            runtime_->stop();
        }
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
    if (login_verify_ != nullptr) {
        login_verify_->stop();
    }
    if (runtime_ != nullptr) runtime_->stop();
    if (started_ && logger_ != nullptr) {
        if (frame_ != nullptr && runtime_ != nullptr) {
            frame_->stopped(*logger_, *runtime_);
        } else if (login_verify_ != nullptr) {
            // login_verify 形态没有 frame,生命周期事件在此补齐配对。
            static_cast<void>(logger_->info(
                "service_stopped", service_name_ + " service stopped"));
        }
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

bool ServiceHost::healthy() const noexcept {
    if (login_verify_ != nullptr) return login_verify_->running();
    return runtime_ != nullptr && runtime_->running();
}

std::optional<std::string> ServiceHost::terminal_error() const {
    if (login_verify_ != nullptr || runtime_ == nullptr) return std::nullopt;
    return runtime_->terminal_error();
}

observability::Logger& ServiceHost::logger() noexcept { return *logger_; }

cluster::ServiceResolver* ServiceHost::resolver() noexcept {
    return resolver_.get();
}

void ServiceHost::tick() {
    if (login_verify_ != nullptr) {
        login_verify_->tick();
    } else if (frame_ != nullptr && runtime_ != nullptr) {
        frame_->tick(*logger_, *runtime_, resolver_.get());
    }
    if (publisher_ == nullptr) return;
    if (!publisher_->tick()) return;
    // required=false 时首注册可能失败,续约成功后补齐 ready。
    const bool running = login_verify_ != nullptr
                             ? login_verify_->running()
                             : runtime_ != nullptr && runtime_->running();
    if (running) {
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
