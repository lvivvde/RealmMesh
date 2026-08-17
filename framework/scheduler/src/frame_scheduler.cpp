#include "realmmesh/scheduler/frame_scheduler.hpp"

#include <chrono>
#include <stdexcept>
#include <thread>

namespace realm::scheduler {
namespace {

std::chrono::nanoseconds calculate_frame_duration(std::uint32_t tick_rate) {
    if (tick_rate == 0) {
        throw std::invalid_argument("tick rate must be greater than zero");
    }

    const auto duration = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::seconds{1}) /
                          tick_rate;
    if (duration.count() == 0) {
        throw std::invalid_argument("tick rate exceeds clock resolution");
    }

    return duration;
}

}  // namespace

IFrameClock::TimePoint SteadyFrameClock::now() const noexcept {
    return std::chrono::steady_clock::now();
}

void SteadyFrameClock::sleep_until(TimePoint deadline) {
    std::this_thread::sleep_until(deadline);
}

FrameScheduler::FrameScheduler(std::uint32_t tick_rate, IFrameClock& clock)
    : tick_rate_(tick_rate),
      frame_duration_(calculate_frame_duration(tick_rate)),
      clock_(clock) {}

std::uint32_t FrameScheduler::tick_rate() const noexcept {
    return tick_rate_;
}

std::chrono::nanoseconds FrameScheduler::frame_duration() const noexcept {
    return frame_duration_;
}

}  // namespace realm::scheduler
