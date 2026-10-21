#pragma once

#include <chrono>
#include <string_view>

namespace realm::observability {

class MetricsRegistry;

/// 作用域计时:构造取单调时钟,析构把经过的秒数记入 histogram。只用于
/// 业务关键边界(服务 tick、外部调用),不给每个函数加。name 须活过计时器
/// (通常是字面量);析构不抛,记账失败时丢弃本次观测。
class ScopedTimer final {
public:
    using Clock = std::chrono::steady_clock;

    ScopedTimer(MetricsRegistry& registry, std::string_view name) noexcept;
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    /// 截至此刻经过的时间(不记账)。
    [[nodiscard]] Clock::duration elapsed() const noexcept;

private:
    MetricsRegistry& registry_;
    std::string_view name_;
    Clock::time_point start_;
};

}  // namespace realm::observability
