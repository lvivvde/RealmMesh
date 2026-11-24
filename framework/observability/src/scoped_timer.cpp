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
    }
}

ScopedTimer::Clock::duration ScopedTimer::elapsed() const noexcept {
    return Clock::now() - start_;
}

}
