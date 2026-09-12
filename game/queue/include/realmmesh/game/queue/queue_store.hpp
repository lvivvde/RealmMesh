#pragma once

#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/game/queue/queue_core.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace realm::game::queue {

/// 排队状态存取的注入缝:etcd 实现(生产)+ fake(测试)。所有方法在
/// 调用线程同步执行(单帧循环内),不引入后台线程。
class QueueStateStore {
public:
    virtual ~QueueStateStore() = default;

    /// 聚合双源额度(§5.2);etcd 不可用或任一前缀失败返回 nullopt
    /// (调用方本批停放,fail-closed)。
    [[nodiscard]] virtual std::optional<BudgetAggregate> refresh_budgets() const = 0;

    /// 快照读取(冷备启动);无快照或 etcd 不可用返回 nullopt(从零开始)。
    [[nodiscard]] virtual std::optional<QueueSnapshot> load_snapshot() const = 0;

    /// 快照写入(随放行批调用);updated_at 为 Unix 秒,仅落快照值、
    /// 不进域状态。失败返回 false(下批重试)。
    [[nodiscard]] virtual bool save_snapshot(
        const QueueSnapshot& snapshot, std::int64_t updated_at_seconds) const = 0;
};

/// etcd v3 HTTP 网关实现。key 契约(与写侧 #43/#46 共享,见 #42 spec):
/// 额度 `/realmmesh/budgets/service/<gateway|realm>/<instance_id>/budget`
/// (值 {conn_free, fetch_free, updated_at};realm 仅 conn_free),快照
/// `/realmmesh/queue/snapshot`(值 {released_number, next_number,
/// admit_rate, updated_at})。
class EtcdQueueStore final : public QueueStateStore {
public:
    struct Options {
        std::string budget_prefix{"/realmmesh/budgets/service"};
        std::string snapshot_key{"/realmmesh/queue/snapshot"};
        std::chrono::milliseconds request_timeout{500};
    };

    EtcdQueueStore(
        Options options, std::shared_ptr<cluster::IEtcdHttpClient> client);

    [[nodiscard]] std::optional<BudgetAggregate> refresh_budgets() const override;
    [[nodiscard]] std::optional<QueueSnapshot> load_snapshot() const override;
    [[nodiscard]] bool save_snapshot(
        const QueueSnapshot& snapshot, std::int64_t updated_at_seconds) const override;

private:
    Options options_;
    std::shared_ptr<cluster::IEtcdHttpClient> client_;
};

}  // namespace realm::game::queue
