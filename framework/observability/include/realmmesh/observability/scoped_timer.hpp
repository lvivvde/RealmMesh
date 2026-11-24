#pragma once

#include <chrono>
#include <string_view>

namespace realm::observability {

class MetricsRegistry;

class ScopedTimer final {
public:
    using Clock = std::chrono::steady_clock;

    ScopedTimer(MetricsRegistry& registry, std::string_view name) noexcept;
    ~ScopedTimer();

    ScopedTimer(const ScopedTimer&) = delete;
    ScopedTimer& operator=(const ScopedTimer&) = delete;

    [[nodiscard]] Clock::duration elapsed() const noexcept;

private:
    MetricsRegistry& registry_;
    std::string_view name_;
    Clock::time_point start_;
};

}
