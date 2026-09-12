#include "realmmesh/game/queue/queue_service.hpp"

#include "realmmesh/network/http/http_server.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace realm::game::queue {
namespace {

/// 环境变量种子(64 hex 字符);缺失或非法即抛(start 失败快速可见)。
[[nodiscard]] common::Ed25519Seed seed_from_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(
            std::string{name} + " is not set");
    }
    return common::parse_identity_seed_hex(value);
}

}  // namespace

QueueService::QueueService(QueueConfig config)
    : config_(std::move(config)) {
    // 生产存取:etcd 客户端与额度前缀/快照键按配置装配(存取注入构造
    // 供测试换 fake;这里成员初始化已完成,直接读 config_)。
    store_ = std::make_unique<EtcdQueueStore>(
        EtcdQueueStore::Options{
            .budget_prefix = config_.budget_prefix,
            .snapshot_key = config_.snapshot_key,
            .request_timeout = std::chrono::milliseconds{500},
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
    ticket_codec_ = std::make_unique<QueueTicketCodec>(
        seed_from_environment("REALMMESH_QUEUE_KEY_SEED"), config_.kid);
    identity_codec_ = std::make_unique<common::IdentityTokenCodec>(
        seed_from_environment("REALMMESH_IDENTITY_KEY_SEED"),
        config_.identity_kid);
    core_ = std::make_unique<QueueCore>(
        config_.release_step, std::chrono::seconds{10}, config_.idempotency_ttl,
        config_.idempotency_capacity);
    handler_ = std::make_unique<QueueHandler>(
        *core_,
        *identity_codec_,
        *ticket_codec_,
        [] { return std::chrono::system_clock::now(); },
        config_.identity_issuer,
        config_.queued_ticket_ttl,
        config_.admit_grace);

    // 冷备恢复(§10):有快照即整体替换水位;无快照或 etcd 不可用从零开始。
    if (const auto snapshot = store_->load_snapshot(); snapshot.has_value()) {
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
    next_release_ = std::chrono::steady_clock::now() + config_.release_interval;

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
    ticket_codec_.reset();
    identity_codec_.reset();
    // store_ 不随停机释放:注入的基础设施(测试 fake / etcd 客户端)
    // 跨 restart 生命周期,冷备恢复在下次 start 仍需读取。
    endpoints_.clear();
}

void QueueService::release_frame() {
    const auto now = std::chrono::system_clock::now();
    // 额度未知(fail-closed)即零额度过阀:本批停放,不误放。
    const BudgetAggregate budgets =
        store_->refresh_budgets().value_or(BudgetAggregate{});
    if (core_->release_batch(budgets, now) == 0) {
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
