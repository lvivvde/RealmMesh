#include "realmmesh/observability/scoped_timer.hpp"

#include "realmmesh/observability/metrics_registry.hpp"

namespace realm::observability {

ScopedTimer::ScopedTimer(
    MetricsRegistry& registry, std::string_view name) noexcept
    : registry_(registry), name_(name), start_(Clock::now()) {}

ScopedTimer::~ScopedTimer() {
    try {
        registry_.histogram_observe(
            name_, std::chrono::duration<double>(elapsed()).count());
    } catch (...) {
        // 观测是旁路记账:分配失败等不影响被计时的业务路径。
    }
}

ScopedTimer::Clock::duration ScopedTimer::elapsed() const noexcept {
    return Clock::now() - start_;
}

}  // namespace realm::observability
