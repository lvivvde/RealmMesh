#pragma once

#include <chrono>
#include <concepts>
#include <cstdint>
#include <functional>
#include <utility>

namespace realm::scheduler {

class IFrameClock {
public:
    using TimePoint = std::chrono::steady_clock::time_point;

    virtual ~IFrameClock() = default;

    [[nodiscard]] virtual TimePoint now() const noexcept = 0;
    virtual void sleep_until(TimePoint deadline) = 0;
};

class SteadyFrameClock final : public IFrameClock {
public:
    [[nodiscard]] TimePoint now() const noexcept override;
    void sleep_until(TimePoint deadline) override;
};

struct FrameContext {
    std::uint64_t index;
    std::chrono::nanoseconds delta;
};

class FrameScheduler {
public:
    FrameScheduler(std::uint32_t tick_rate, IFrameClock& clock);

    [[nodiscard]] std::uint32_t tick_rate() const noexcept;
    [[nodiscard]] std::chrono::nanoseconds frame_duration() const noexcept;

    template <typename Callback>
        requires std::invocable<Callback&, FrameContext> &&
                 std::convertible_to<std::invoke_result_t<Callback&, FrameContext>, bool>
    std::uint64_t run(Callback&& callback) {
        auto next_deadline = clock_.now();
        std::uint64_t frame_index = 0;

        while (true) {
            ++frame_index;
            const FrameContext context{frame_index, frame_duration_};
            if (!static_cast<bool>(std::invoke(callback, context))) {
                return frame_index;
            }

            next_deadline += frame_duration_;
            clock_.sleep_until(next_deadline);
        }
    }

private:
    std::uint32_t tick_rate_;
    std::chrono::nanoseconds frame_duration_;
    IFrameClock& clock_;
};

}  // namespace realm::scheduler
