#pragma once

#include "realmmesh/cluster/service_registry.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace realm::cluster {

/// 额度 key 前缀(主 spec §5.2);排队服按
/// `<前缀>/<gateway|realm>/` 聚合,与 queue 侧 budget_prefix 默认值一致。
inline constexpr std::string_view budget_key_prefix{"/realmmesh/budgets/service"};

/// 一个实例的额度快照:conn_free 必有;fetch_free 仅网关拉取管线
/// 存在(realm 置 has_fetch=false,只评估连接额度)。
struct InstanceBudgetSnapshot {
    std::uint64_t conn_free{0};
    std::uint64_t fetch_free{0};
    bool has_fetch{false};

    bool operator==(const InstanceBudgetSnapshot&) const = default;
};

/// 额度 key:`<budget_key_prefix>/<type>/<instance_id>/budget`。
[[nodiscard]] std::string budget_key(
    ServiceType type, std::string_view instance_id);

/// 阈值发布策略(主 spec §5.2:±10% 或 ≥1s 间隔),纯域策略(不含 IO):
/// 首次发布恒发布;无变化不发布;变化超阈值立即发布;归零/回满(free
/// 回到容量)立即发布;其余变化等满间隔。容量未知(0)时只按归零与
/// 阈值判定。
struct BudgetPublishPolicy {
    double change_threshold{0.10};
    std::chrono::milliseconds min_interval{1000};
    /// 回满判定基准:conn/fetch 的容量上限;0 = 未知,跳过回满判定。
    std::uint64_t conn_capacity{0};
    std::uint64_t fetch_capacity{0};

    /// confirmed 为上次已发布到 etcd 的快照(nullopt = 尚未发布);
    /// confirmed_at 仅在 confirmed 存在时参与间隔判定。
    [[nodiscard]] bool should_publish(
        const std::optional<InstanceBudgetSnapshot>& confirmed,
        const InstanceBudgetSnapshot& next,
        std::chrono::system_clock::time_point confirmed_at,
        std::chrono::system_clock::time_point now) const;
};

/// 实例额度上报器:阈值策略 + 租约绑定写(put_leased)。线程约定:
/// 归调用方线程(网关=业务帧/Realm=各自帧);etcd 写失败不更新已发布
/// 状态,后续 publish 按节流间隔自动重试,写失败经 failure_sink 通知
/// (若设置)由调用方记录错误。
class InstanceBudgetReporter final {
public:
    InstanceBudgetReporter(
        IServiceRegistry& registry,
        RegistrationId registration_id,
        ServiceType type,
        std::string instance_id,
        BudgetPublishPolicy policy = {});

    /// 写失败回调(可缺省):put_leased 真实写失败时以额度 key 调用
    /// 恰好一次;节流中的重试未触达存储,不回调。调用方借此记录错误
    /// (spec #43:发布失败记录错误,下次阈值满足时重试)。
    void set_failure_sink(std::function<void(const std::string&)> sink);

    /// 快照进 → 策略判定 → put_leased;返回本次是否发布成功。
    /// 未发布(无变化/节流中/写失败)时状态不变,下次调用自然重试。
    [[nodiscard]] bool publish(
        const InstanceBudgetSnapshot& snapshot,
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now());

private:
    IServiceRegistry* registry_;
    RegistrationId registration_id_;
    ServiceType type_;
    std::string instance_id_;
    BudgetPublishPolicy policy_;
    std::optional<InstanceBudgetSnapshot> confirmed_;
    std::optional<std::chrono::system_clock::time_point> confirmed_at_;
    std::optional<InstanceBudgetSnapshot> attempted_;
    std::optional<std::chrono::system_clock::time_point> attempted_at_;
    bool attempted_ok_{false};
    std::function<void(const std::string&)> failure_sink_;
};

}  // namespace realm::cluster
