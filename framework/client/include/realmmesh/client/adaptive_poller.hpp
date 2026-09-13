#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>

namespace realm::client {

/// 分档轮询参数(spec §7 初值):初始 2s;位次 >1000 → 5s;≤10 → 1s;
/// 其余 2s;±20% 抖动;连续 3 次失败起指数退避、30s 封顶。
struct PollTierConfig final {
    /// 位次高于此值走 far_interval(规格:>1000 → 5s)。
    std::uint64_t far_position{1000};
    /// 位次不高于此值走 near_interval(规格:≤10 → 1s)。
    std::uint64_t near_position{10};
    std::chrono::milliseconds initial_interval{2000};
    std::chrono::milliseconds far_interval{5000};
    std::chrono::milliseconds near_interval{1000};
    /// 连续失败达到此数起指数退避(规格:连续 3 次失败)。
    std::uint32_t backoff_after_failures{3};
    std::chrono::milliseconds backoff_cap{30000};
    /// 间隔硬下限:progress 的 Cache-Control max-age(规格 §7、ADR-0006
    /// 缓存 1~2s)。位次再近也不得比缓存更新更快。
    std::chrono::milliseconds min_interval{1000};
    /// 抖动幅度(规格 ±20%)。
    double jitter_ratio{0.2};
};

/// 抖动源:返回 [0,1) 均匀随机数;测试注入确定性序列。
using JitterSource = std::function<double()>;

[[nodiscard]] JitterSource make_random_jitter_source();

/// 自适应轮询节奏(spec §7):位次分档 + ±20% 抖动 + 连续失败指数退避
/// (成功即复位)。纯逻辑,无 IO;链路把它算出的间隔用于 progress 轮询。
class AdaptivePoller final {
public:
    explicit AdaptivePoller(
        PollTierConfig config = {},
        JitterSource jitter = make_random_jitter_source());

    /// 下一次轮询的等待时长。失败计数 ≥ 阈值时按 2^(失败数-阈值+1) 翻倍,
    /// 抖动后封顶 backoff_cap;下限 min_interval。
    [[nodiscard]] std::chrono::milliseconds next_interval(
        std::uint64_t position) const;

    void record_success() noexcept;
    void record_failure() noexcept;
    [[nodiscard]] std::uint32_t consecutive_failures() const noexcept {
        return consecutive_failures_;
    }

    /// ETA 本地插值(spec §7、ADR-0006:位次/ETA 客户端本地算):
    /// 剩余位次 / admit_rate;速率未知(≤0)时无估计。
    [[nodiscard]] static std::optional<std::chrono::seconds> estimate_eta(
        std::uint64_t position, double admit_rate);

private:
    [[nodiscard]] std::chrono::milliseconds tier_interval(
        std::uint64_t position) const;
    [[nodiscard]] std::chrono::milliseconds apply_jitter(
        std::chrono::milliseconds base) const;

    PollTierConfig config_;
    JitterSource jitter_;
    std::uint32_t consecutive_failures_{0};
};

}  // namespace realm::client
