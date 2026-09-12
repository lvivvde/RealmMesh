#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace realm::game::queue {

/// 网关/业务服实例上报的额度(§5.2 etcd 值 schema;realm 仅 conn_free)。
struct GatewayBudget {
    std::uint64_t conn_free;
    std::uint64_t fetch_free;
};

struct RealmBudget {
    std::uint64_t conn_free;
};

/// 放行阀门的输入聚合:网关侧按实例 min(conn_free, fetch_free) 求和
/// (对 spec Σ网关 fetch_free/conn_free 的裁决——sum 级取 min 会在实例
/// 间额度错配时过量放行),业务服侧 Σconn_free。
struct BudgetAggregate {
    std::uint64_t gateway_admission{0};
    std::uint64_t realm_connections{0};

    bool operator==(const BudgetAggregate&) const = default;
};

[[nodiscard]] BudgetAggregate aggregate_budgets(
    std::span<const GatewayBudget> gateways,
    std::span<const RealmBudget> realms);

/// 冷备快照(§10):两个水位 + 实测放行速率。
struct QueueSnapshot {
    std::uint64_t released_number{0};
    std::uint64_t next_number{1};
    std::uint64_t admit_rate{0};

    bool operator==(const QueueSnapshot&) const = default;
};

/// 放行速率实测固定窗(ADR-0006 定值);构造默认值与装配侧共用。
inline constexpr std::chrono::seconds queue_rate_window{10};

/// 排队权威状态(ADR-0006):两个水位 + 实测放行速率 + 发号幂等映射。
/// 纯域逻辑,无 IO;时间一律由调用方注入,etcd 存取在 QueueStateStore。
class QueueCore final {
public:
    struct Issued {
        std::uint64_t number;
        /// false = 同一身份会话(jti)的幂等重放。
        bool fresh;

        bool operator==(const Issued&) const = default;
    };

    /// release_step:放行步长(0 = 配置关阀);rate_window:放行速率
    /// 实测固定窗;idempotency_ttl/capacity:幂等映射条目的过期与上限
    /// (超限先清扫过期条目,发号不因此失败)。
    explicit QueueCore(
        std::uint64_t release_step,
        std::chrono::seconds rate_window = queue_rate_window,
        std::chrono::seconds idempotency_ttl = std::chrono::seconds{1800},
        std::size_t idempotency_capacity = 1'000'000);

    /// 发号(幂等):同一 identity_jti 拿同号。
    [[nodiscard]] Issued issue(
        std::string identity_jti,
        std::chrono::system_clock::time_point now);

    [[nodiscard]] std::uint64_t released_number() const noexcept;
    [[nodiscard]] std::uint64_t next_number() const noexcept;

    /// 实测放行速率(号/秒,floor;窗内无放行为 0)。只读不修剪。
    [[nodiscard]] std::uint64_t admit_rate(
        std::chrono::system_clock::time_point now) const noexcept;

    /// 放行一批(定时帧调用):min(网关侧额度, Σ业务服 conn_free, 步长,
    /// 已发未放行余量);额度未知侧传 0 即本批停放(fail-closed)。返回
    /// 本批实际放行量(可为 0)。
    [[nodiscard]] std::uint64_t release_batch(
        const BudgetAggregate& budgets,
        std::chrono::system_clock::time_point now);

    /// 冷备恢复:两个水位整体替换;released ≥ next 视为损坏快照抛出。
    void restore(const QueueSnapshot& snapshot);

    /// 当前水位(供快照落盘;速率取 as-of now)。
    [[nodiscard]] QueueSnapshot snapshot(
        std::chrono::system_clock::time_point now) const;

private:
    struct ReleaseRecord {
        std::chrono::system_clock::time_point at;
        std::uint64_t amount;
    };

    struct IdempotencyEntry {
        std::uint64_t number;
        std::chrono::system_clock::time_point issued_at;
    };

    void prune_releases(std::chrono::system_clock::time_point now);

    std::uint64_t release_step_;
    std::chrono::seconds rate_window_;
    std::chrono::seconds idempotency_ttl_;
    std::size_t idempotency_capacity_;
    /// 两个水位以原子承载(spec §5.1 对外只读量);域操作仍限单帧循环
    /// 线程,原子仅保证观测方读到的水位不撕裂。
    std::atomic<std::uint64_t> released_number_{0};
    std::atomic<std::uint64_t> next_number_{1};
    std::deque<ReleaseRecord> releases_;
    std::unordered_map<std::string, IdempotencyEntry> idempotency_;
};

}  // namespace realm::game::queue
