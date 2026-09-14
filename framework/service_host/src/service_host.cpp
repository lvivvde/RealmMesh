#include "realmmesh/service_host/service_host.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/cluster/service_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/game/login_verify/login_verify_service.hpp"
#include "realmmesh/game/queue/queue_service.hpp"
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

/// 依赖解析对象映射:gateway→Realm(#45:handoff 直连端点来自 Realm
/// 发现,缺失退回静态下游)、realm→Gateway;login_verify/queue 无下游
/// 依赖(HTTPS 服务,状态经 etcd 而非服务依赖)。
[[nodiscard]] std::optional<cluster::ServiceType> dependency_service_type(
    std::string_view service_name) {
    if (service_name == "gateway") return cluster::ServiceType::Realm;
    if (service_name == "realm") return cluster::ServiceType::Gateway;
    if (service_name == "login_verify") return std::nullopt;
    if (service_name == "queue") return std::nullopt;
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
    // login_verify 与 queue 是 HTTPS 请求循环形态:宿主级节段仍走
    // GatewayConfig 的解析面,业务体换成各自的 Service(无 ServiceFrame)。
    std::unique_ptr<game::login_verify::LoginVerifyService> login_verify;
    std::unique_ptr<game::queue::QueueService> queue;
    game::gateway::GatewayConfig config;
    if (service_name_ == "login_verify") {
        auto login_verify_config = LayeredConfigLoader::load_login_verify(
            config_root, service_name, overrides);
        login_verify =
            std::make_unique<game::login_verify::LoginVerifyService>(
                std::move(login_verify_config.login_verify),
                &metrics_registry_);
        config = std::move(login_verify_config.host);
    } else if (service_name_ == "queue") {
        auto queue_config = LayeredConfigLoader::load_queue(
            config_root, service_name, overrides);
        queue = std::make_unique<game::queue::QueueService>(
            std::move(queue_config.queue), &metrics_registry_);
        config = std::move(queue_config.host);
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
    if (login_verify != nullptr || queue != nullptr) {
        login_verify_ = std::move(login_verify);
        queue_ = std::move(queue);
        return;
    }
    // conn 容量与传输层同源(#43):取全部启用传输的 max_sessions 之和
    // (QUIC/TLS 竞速下两条传输可同时满载),管线满即传输满,满额拒绝
    // 语义不依赖两处配置保持一致。
    std::uint64_t pipeline_conn_capacity = 0;
    for (const auto& transport : config.transports) {
        if (transport.enabled) {
            pipeline_conn_capacity += transport.max_sessions;
        }
    }
    frame_ = std::make_unique<ServiceFrame>(
        service_name_,
        config.downstream_address,
        config.downstream_port,
        config.max_events_per_frame,
        EdgePipelineCaps{
            pipeline_conn_capacity,
            // fetch 容量仅 gateway 拉取管线存在,其余身份(含 realm)
            // 置 0,与快照 has_fetch=false 同一不变量。
            service_name_ == "gateway" ? config.pipeline_fetch_capacity
                                       : 0},
        EdgePipelineTuning{
            std::chrono::milliseconds{config.fetch_retry_base_ms},
            config.fetch_retry_max,
            std::chrono::milliseconds{config.handoff_grace_ms}},
        nullptr,
        &metrics_registry_);
    budget_policy_.conn_capacity = pipeline_conn_capacity;
    // fetch 容量是回满判定基准,仅 gateway 拉取管线存在;realm 无
    // fetch 维度(§5.2 只上报 conn_free),置 0 与快照 has_fetch=false
    // 保持一致。
    budget_policy_.fetch_capacity =
        service_name_ == "gateway" ? config.pipeline_fetch_capacity : 0;
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
    } else if (queue_ != nullptr) {
        queue_->start(logger_.get());
        running = queue_->running();
    } else {
        runtime_->start();
        running = runtime_->running();
    }
    if (!running) return false;
    try {
        if (discovery_config_.enabled) {
            // 重启场景按引用链反序清理旧装配,避免 publisher 悬挂 registry。
            budget_reporter_.reset();
            resolver_.reset();
            publisher_.reset();
            registry_.reset();
            registry_ = std::make_unique<cluster::EtcdServiceRegistry>(
                cluster::make_etcd_registry_options(discovery_config_));
            // 服务注册端点:HTTPS 形态来自各自 Service,消息形态来自 runtime。
            std::span<const network::TransportEndpoint> registered_endpoints;
            if (login_verify_ != nullptr) {
                registered_endpoints = login_verify_->local_endpoints();
            } else if (queue_ != nullptr) {
                registered_endpoints = queue_->local_endpoints();
            } else {
                registered_endpoints = runtime_->local_endpoints();
            }
            publisher_ = std::make_unique<cluster::ServicePublisher>(
                *registry_,
                cluster::make_service_instance(
                    *self_type,
                    discovery_config_,
                    registered_endpoints,
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
        } else if (queue_ != nullptr) {
            queue_->stop();
        } else if (runtime_ != nullptr) {
            runtime_->stop();
        }
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
    if (login_verify_ != nullptr) {
        login_verify_->stop();
    }
    if (queue_ != nullptr) queue_->stop();
    if (runtime_ != nullptr) runtime_->stop();
    if (started_ && logger_ != nullptr) {
        if (frame_ != nullptr && runtime_ != nullptr) {
            frame_->stopped(*logger_, *runtime_);
        } else if (login_verify_ != nullptr || queue_ != nullptr) {
            // HTTPS 形态没有 frame,生命周期事件在此补齐配对。
            static_cast<void>(logger_->info(
                "service_stopped", service_name_ + " service stopped"));
        }
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

game::gateway::GatewayRuntime& ServiceHost::runtime() noexcept {
    return *runtime_;
}

bool ServiceHost::healthy() const noexcept {
    if (login_verify_ != nullptr) return login_verify_->running();
    if (queue_ != nullptr) return queue_->running();
    return runtime_ != nullptr && runtime_->running();
}

std::optional<std::string> ServiceHost::terminal_error() const {
    if (login_verify_ != nullptr || queue_ != nullptr || runtime_ == nullptr) {
        return std::nullopt;
    }
    return runtime_->terminal_error();
}

observability::Logger& ServiceHost::logger() noexcept { return *logger_; }

cluster::ServiceResolver* ServiceHost::resolver() noexcept {
    return resolver_.get();
}

void ServiceHost::tick() {
    if (login_verify_ != nullptr) {
        login_verify_->tick();
    } else if (queue_ != nullptr) {
        queue_->tick();
    } else if (frame_ != nullptr && runtime_ != nullptr) {
        frame_->tick(
            *logger_, *runtime_, resolver_.get(), budget_reporter_.get());
    }
    if (publisher_ == nullptr) return;
    if (!publisher_->tick()) return;
    // 注册成功后装配额度上报器(#43 gateway / #46 realm):required=false
    // 时首注册可能失败,注册成功后的首个 tick 补齐。名字→身份的映射
    // 只由 parse_service_identity 持有,这里按身份判定上报资格。
    auto budget_type = parse_service_identity(service_name_);
    if (budget_type != cluster::ServiceType::Gateway &&
        budget_type != cluster::ServiceType::Realm) {
        budget_type.reset();
    }
    if (budget_reporter_ == nullptr && budget_type.has_value() &&
        publisher_->registered()) {
        budget_reporter_ = std::make_unique<cluster::InstanceBudgetReporter>(
            *registry_,
            publisher_->registration_id(),
            *budget_type,
            instance_,
            budget_policy_);
        budget_reporter_->set_failure_sink([this](const std::string& key) {
            static_cast<void>(logger_->warn(
                "instance_budget_publish_failed",
                "instance budget publish failed; will retry when policy allows",
                {observability::field("key", key)}));
        });
    }
    // required=false 时首注册可能失败,续约成功后补齐 ready。
    const bool running = login_verify_ != nullptr
                             ? login_verify_->running()
                             : queue_ != nullptr
                                 ? queue_->running()
                                 : runtime_ != nullptr && runtime_->running();
    if (running) {
        ready_.store(true);
    }
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
