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

/// 一帧的实际耗时:work 是帧回调本身的耗时(不含帧间睡眠),budget 是
/// 帧周期;work 超过 budget 即超帧,后续帧的截止时间随之滞后。
struct FrameTiming {
    std::uint64_t index;
    std::chrono::nanoseconds work;
    std::chrono::nanoseconds budget;

    [[nodiscard]] bool overrun() const noexcept { return work > budget; }
};

/// 每帧回调返回后调用(含返回 false 的末帧)。调度器只测量、不记账:
/// 指标如何落盘由装配方决定,framework/scheduler 不依赖 observability。
using FrameObserver = std::function<void(const FrameTiming&)>;

class FrameScheduler {
public:
    FrameScheduler(std::uint32_t tick_rate, IFrameClock& clock);

    [[nodiscard]] std::uint32_t tick_rate() const noexcept;
    [[nodiscard]] std::chrono::nanoseconds frame_duration() const noexcept;

    /// 设置帧耗时观察者;空函数即关闭。须在 run 之前设置。
    void set_frame_observer(FrameObserver observer);

    template <typename Callback>
        requires std::invocable<Callback&, FrameContext> &&
                 std::convertible_to<
                     std::invoke_result_t<Callback&, FrameContext>,
                     bool>
    std::uint64_t run(Callback&& callback) {
        auto next_deadline = clock_.now();
        std::uint64_t frame_index = 0;

        while (true) {
            ++frame_index;
            const FrameContext context{frame_index, frame_duration_};
            const auto work_start = clock_.now();
            const bool keep_running =
                static_cast<bool>(std::invoke(callback, context));
            if (observer_) {
                observer_(FrameTiming{
                    frame_index,
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        clock_.now() - work_start),
                    frame_duration_});
            }
            if (!keep_running) {
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
    FrameObserver observer_;
};

}  // namespace realm::scheduler
