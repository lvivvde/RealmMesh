#include "realmmesh/game/queue/queue_service.hpp"

#include "realmmesh/network/http/http_server.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <utility>

namespace realm::game::queue {

QueueService::QueueService(QueueConfig config)
    : config_(std::move(config)) {
    // 生产存取:etcd 客户端与额度前缀/快照键按配置装配(存取注入构造
    // 供测试换 fake;这里成员初始化已完成,直接读 config_)。
    store_ = std::make_shared<EtcdQueueStore>(
        EtcdQueueStore::Options{
            .budget_prefix = config_.budget_prefix,
            .snapshot_key = config_.snapshot_key,
        },
        cluster::make_etcd_http_client(
            config_.etcd_endpoint, std::chrono::milliseconds{500}));
}

QueueService::QueueService(
    QueueConfig config, std::shared_ptr<QueueStateStore> store)
    : config_(std::move(config)),
      store_(std::move(store)) {}

QueueService::~QueueService() = default;

void QueueService::start(observability::Logger* logger) {
    // 号牌签发键与身份验签键分置:验签走健全服发布的身份种子,签发走
    // queue 自己的种子(kid 在 JWKS 语义上由签发方独占)。
    // TODO(#42):号牌签发私钥只应存在于排队调度服;当前经环境变量分发
    // 属过渡形态,生产由密钥管理注入并收敛到本服务。
    number_codec_ = std::make_unique<common::QueueNumberCodec>(
        common::seed_from_environment("REALMMESH_QUEUE_KEY_SEED"),
        config_.kid);
    // TODO(#42):身份验签当前信任环境变量注入的对称种子;生产应改为
    // 消费 login_verify 发布的 JWKS/公钥并支持轮换。
    identity_codec_ = std::make_unique<common::IdentityTokenCodec>(
        common::seed_from_environment("REALMMESH_IDENTITY_KEY_SEED"),
        config_.identity_kid);
    core_ = std::make_unique<QueueCore>(
        config_.release_step, queue_rate_window, config_.idempotency_ttl,
        config_.idempotency_capacity);
    handler_ = std::make_unique<QueueHandler>(
        *core_,
        *identity_codec_,
        *number_codec_,
        [] { return std::chrono::system_clock::now(); },
        config_.identity_issuer,
        config_.queued_number_ttl,
        config_.admit_grace);

    // 冷备恢复(§10):有快照即整体替换水位;无快照(确属空状态)从零
    // 开始;etcd 不可达或快照损坏抛出——不得在未知水位上从零重发存量
    // 号。snapshot_required=false(开发网状无 etcd)时降级为告警放行。
    try {
        if (const auto snapshot = store_->load_snapshot();
            snapshot.has_value()) {
            core_->restore(*snapshot);
            if (logger != nullptr) {
                static_cast<void>(logger->info(
                    "queue_snapshot_restored",
                    "queue state restored from snapshot",
                    {observability::field(
                         "released_number",
                         static_cast<std::int64_t>(snapshot->released_number)),
                     observability::field(
                         "next_number",
                         static_cast<std::int64_t>(snapshot->next_number))}));
            }
        }
    } catch (const std::exception& error) {
        if (config_.snapshot_required) {
            throw;
        }
        if (logger != nullptr) {
            static_cast<void>(logger->warn(
                "queue_snapshot_unavailable",
                std::string{"queue snapshot unavailable, starting from zero: "} +
                    error.what()));
        }
    }

    network::HttpServerConfig http_config;
    http_config.tls_identity = config_.tls;
    server_ = std::make_unique<network::HttpServer>(
        config_.listen_address, config_.listen_port, http_config,
        [this](const network::Http1Request& request) {
            return handler_->handle(request);
        });
    endpoints_ = {network::TransportEndpoint{
        .name = "https",
        .protocol = network::TransportProtocol::TlsTcp,
        .address = config_.listen_address,
        .port = server_->local_port()}};
    const auto now = std::chrono::steady_clock::now();
    next_budget_poll_ = now + config_.budget_interval;
    next_release_ = now + config_.release_interval;

    if (logger != nullptr) {
        static_cast<void>(logger->info(
            "listener_started",
            "queue listener started",
            {observability::field("listen_address", config_.listen_address),
             observability::field("listen_port", server_->local_port()),
             observability::field(
                 "transport",
                 network::to_string(network::TransportProtocol::TlsTcp)),
             observability::field("transport_name", std::string("https"))}));
        static_cast<void>(logger->info("service_started", "queue service started"));
    }
}

void QueueService::stop() {
    server_.reset();
    handler_.reset();
    core_.reset();
    number_codec_.reset();
    identity_codec_.reset();
    budgets_.reset();
    // store_ 不随停机释放:注入的基础设施(测试 fake / etcd 客户端)
    // 跨 restart 生命周期,冷备恢复在下次 start 仍需读取。
    endpoints_.clear();
}

void QueueService::release_frame() {
    const auto now = std::chrono::system_clock::now();
    // 额度取最近一次额度帧的缓存(独立按 budget_interval 轮询);未知
    // (fail-closed)即零额度过阀:本批停放,不误放。
    if (core_->release_batch(budgets_.value_or(BudgetAggregate{}), now) == 0) {
        return;
    }
    static_cast<void>(store_->save_snapshot(
        core_->snapshot(now),
        std::chrono::duration_cast<std::chrono::seconds>(now.time_since_epoch())
            .count()));
}

void QueueService::tick() {
    if (server_ == nullptr) {
        return;
    }
    server_->poll_once(std::chrono::milliseconds(2));
    const auto now = std::chrono::steady_clock::now();
    if (now >= next_budget_poll_) {
        next_budget_poll_ = now + config_.budget_interval;
        // 刷新失败保持 nullopt:阀门继续按零额度停放(fail-closed)。
        budgets_ = store_->refresh_budgets();
    }
    if (now >= next_release_) {
        next_release_ = now + config_.release_interval;
        release_frame();
    }
}

bool QueueService::running() const noexcept {
    return server_ != nullptr;
}

const std::vector<network::TransportEndpoint>&
QueueService::local_endpoints() const noexcept {
    return endpoints_;
}

}  // namespace realm::game::queue
