#pragma once

#include "realmmesh/game/queue/queue_config.hpp"
#include "realmmesh/game/queue/queue_core.hpp"
#include "realmmesh/game/queue/queue_handler.hpp"
#include "realmmesh/game/queue/queue_store.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/network/transport/message_transport.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <vector>

namespace realm::network {
class HttpServer;
}  // namespace realm::network

namespace realm::observability {
class Logger;
class MetricsRegistry;
}  // namespace observability

namespace realm::game::queue {

/// 排队调度服运行体(ADR-0006):HTTPS 服务边 + QueueCore 权威状态 +
/// etcd 状态存取的装配。start 读取双种子环境变量、恢复冷备快照并绑定
/// 监听;tick 驱动服务边轮询与放行阀门定时帧(额度轮询 + 批放行 +
/// 快照落盘),由 ServiceHost 每帧调用。
class QueueService final {
public:
    /// metrics(#47):宿主持有的指标注册表;可空(测试装配)。
    explicit QueueService(
        QueueConfig config,
        observability::MetricsRegistry* metrics = nullptr);
    /// 存取注入构造(测试用 fake store;生产路径走 etcd)。shared_ptr
    /// 语义:测试侧需在 stop() 清空后仍持引用观测 save 调用。
    QueueService(
        QueueConfig config,
        std::shared_ptr<QueueStateStore> store,
        observability::MetricsRegistry* metrics = nullptr);
    ~QueueService();

    QueueService(const QueueService&) = delete;
    QueueService& operator=(const QueueService&) = delete;

    /// 装载 REALMMESH_QUEUE_KEY_SEED(号牌签发)与
    /// REALMMESH_IDENTITY_KEY_SEED(身份验签)并绑定监听;种子缺失
    /// 抛异常,成功返回即存活。logger 可为空(测试装配)。
    void start(observability::Logger* logger = nullptr);
    void stop();

    /// 驱动一轮服务边轮询与放行定时帧;未启动时为空操作。
    void tick();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const std::vector<network::TransportEndpoint>&
    local_endpoints() const noexcept;

private:
    /// 单个放行帧:取缓存的额度聚合(fail-closed)→ 批放行 → 快照落盘。
    void release_frame();
    /// 帧尾指标发布(#47):取号/水位/速率/队列估算进注册表(轮询
    /// 读权威核心,域内核零污染);registry 为空时是空操作。
    void publish_metrics();

    QueueConfig config_;
    observability::MetricsRegistry* metrics_{nullptr};
    std::shared_ptr<QueueStateStore> store_;
    std::unique_ptr<common::IdentityTokenCodec> identity_codec_;
    std::unique_ptr<common::QueueNumberCodec> number_codec_;
    std::unique_ptr<QueueCore> core_;
    std::unique_ptr<QueueHandler> handler_;
    std::unique_ptr<network::HttpServer> server_;
    std::vector<network::TransportEndpoint> endpoints_;
    /// 最近一次成功轮询的额度聚合;nullopt = 额度未知(停放)。
    std::optional<BudgetAggregate> budgets_;
    std::chrono::steady_clock::time_point next_budget_poll_{};
    std::chrono::steady_clock::time_point next_release_{};
};

}  // namespace realm::game::queue
