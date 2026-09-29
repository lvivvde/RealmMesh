#pragma once

#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/game/queue/queue_core.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace realm::game::queue {

enum class QueueIssueStatus {
    Issued,
    Recovered,
    Unavailable,
};

struct QueueIssueRequest final {
    std::string identity_jti;
    std::chrono::system_clock::time_point issued_at;
    std::chrono::system_clock::time_point identity_expires_at;
    QueueSnapshot snapshot;
};

struct QueueIssueResult final {
    QueueIssueStatus status{QueueIssueStatus::Unavailable};
    std::uint64_t number{0};
    std::chrono::system_clock::time_point issued_at;
    QueueSnapshot snapshot;
};

/// 排队状态存取的注入缝:etcd 实现(生产)+ fake(测试)。所有方法在
/// 调用线程同步执行(单帧循环内),不引入后台线程。
class QueueStateStore {
public:
    virtual ~QueueStateStore() = default;

    /// 聚合双源额度(§5.2);etcd 不可用或任一前缀失败返回 nullopt
    /// (调用方本批停放,fail-closed)。
    [[nodiscard]] virtual std::optional<BudgetAggregate> refresh_budgets() const = 0;

    /// 快照读取(冷备启动)的三态契约:无快照返回 nullopt(确属空
    /// 状态,从零开始);etcd 不可达或快照损坏抛出(std::runtime_error,
    /// 启动失败——冷备未知时从零重发会与存量号牌冲突,fail-closed)。
    [[nodiscard]] virtual std::optional<QueueSnapshot> load_snapshot() const = 0;

    /// 原子发号/找回：新登录尝试在同一事务内提交 identity_jti 映射与
    /// next_number 水位；重复尝试返回原号码。Unavailable 表示不得确认
    /// 发号，调用方应返回可重试失败。
    [[nodiscard]] virtual QueueIssueResult issue_or_recover(
        const QueueIssueRequest& request) const = 0;

    /// 快照写入(随放行批调用);updated_at 为 Unix 秒,仅落快照值、
    /// 不进域状态。失败返回 false(下批重试)。
    [[nodiscard]] virtual bool save_snapshot(
        const QueueSnapshot& snapshot, std::int64_t updated_at_seconds) const = 0;
};

/// etcd v3 HTTP 网关实现。key 契约(与写侧 #43/#46 共享,见 #42 spec):
/// 额度 `/realmmesh/budgets/service/<gateway|realm>/<instance_id>/budget`
/// (值 {conn_free, fetch_free, updated_at};realm 仅 conn_free),快照
/// `/realmmesh/queue/snapshot`(值含 released_number、next_number、
/// admit_rate、release_batches_pruned_through、release_batches 与
/// updated_at),以及 `/realmmesh/queue/issuance/<jti-digest>` 的未过期
/// 发号恢复映射。新映射与递增快照在同一事务写入。
class EtcdQueueStore final : public QueueStateStore {
public:
    struct Options {
        std::string budget_prefix{"/realmmesh/budgets/service"};
        std::string snapshot_key{"/realmmesh/queue/snapshot"};
        std::string issuance_prefix{"/realmmesh/queue/issuance"};
    };

    EtcdQueueStore(
        Options options, std::shared_ptr<cluster::IEtcdHttpClient> client);

    [[nodiscard]] std::optional<BudgetAggregate> refresh_budgets() const override;
    [[nodiscard]] std::optional<QueueSnapshot> load_snapshot() const override;
    [[nodiscard]] QueueIssueResult issue_or_recover(
        const QueueIssueRequest& request) const override;
    [[nodiscard]] bool save_snapshot(
        const QueueSnapshot& snapshot, std::int64_t updated_at_seconds) const override;

private:
    Options options_;
    std::shared_ptr<cluster::IEtcdHttpClient> client_;
    mutable std::unordered_map<std::int64_t, std::int64_t> issuance_leases_;
    /// 发号事务的提交结果既无应答也无法回读时，本进程的内存水位已不再
    /// 能证明与 etcd 一致。此后永久拒绝发号，直到重启并重新加载权威状态。
    mutable bool issuance_state_uncertain_{false};
};

}  // namespace realm::game::queue
