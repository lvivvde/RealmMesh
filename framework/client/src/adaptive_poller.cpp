#include "realmmesh/client/adaptive_poller.hpp"

#include <algorithm>
#include <random>

namespace realm::client {
namespace {

[[nodiscard]] std::chrono::milliseconds clamp_interval(
    std::chrono::milliseconds value,
    std::chrono::milliseconds min_interval,
    std::chrono::milliseconds max_interval) {
    if (value < min_interval) {
        return min_interval;
    }
    if (value > max_interval) {
        return max_interval;
    }
    return value;
}

}  // namespace

JitterSource make_random_jitter_source() {
    return [] {
        static thread_local std::mt19937_64 engine{std::random_device{}()};
        static thread_local std::uniform_real_distribution<double> distribution{
            0.0, 1.0};
        return distribution(engine);
    };
}

AdaptivePoller::AdaptivePoller(PollTierConfig config, JitterSource jitter)
    : config_(config), jitter_(std::move(jitter)) {}

std::chrono::milliseconds AdaptivePoller::tier_interval(
    std::uint64_t position) const {
    if (position > config_.far_position) {
        return config_.far_interval;
    }
    if (position <= config_.near_position) {
        return config_.near_interval;
    }
    return config_.initial_interval;
}

std::chrono::milliseconds AdaptivePoller::apply_jitter(
    std::chrono::milliseconds base) const {
    if (jitter_ == nullptr || config_.jitter_ratio <= 0.0) {
        return base;
    }
    const double unit = std::clamp(jitter_(), 0.0, 1.0);
    const double scale =
        1.0 - config_.jitter_ratio + 2.0 * config_.jitter_ratio * unit;
    return std::chrono::milliseconds{static_cast<std::int64_t>(
        static_cast<double>(base.count()) * scale)};
}

std::chrono::milliseconds AdaptivePoller::next_interval(
    std::uint64_t position) const {
    auto base = tier_interval(position);
    if (consecutive_failures_ >= config_.backoff_after_failures) {
        const auto doublings =
            consecutive_failures_ - config_.backoff_after_failures + 1U;
        // 逐次翻倍(不左移越界:超过封顶即停)。
        for (std::uint32_t step = 0; step < doublings; ++step) {
            if (base >= config_.backoff_cap) {
                break;
            }
            base = std::min(base * 2, config_.backoff_cap);
        }
    }
    // 抖动后再封顶:封顶是硬上限,抖动不能把它顶破。
    return clamp_interval(
        apply_jitter(base), config_.min_interval, config_.backoff_cap);
}

void AdaptivePoller::record_success() noexcept { consecutive_failures_ = 0; }

void AdaptivePoller::record_failure() noexcept {
    if (consecutive_failures_ < 0xFFFFU) {
        ++consecutive_failures_;
    }
}

std::optional<std::chrono::seconds> AdaptivePoller::estimate_eta(
    std::uint64_t position, double admit_rate) {
    if (admit_rate <= 0.0) {
        return std::nullopt;
    }
    const auto seconds =
        static_cast<std::int64_t>(static_cast<double>(position) / admit_rate);
    return std::chrono::seconds{std::max<std::int64_t>(seconds, 0)};
}

}  // namespace realm::client
