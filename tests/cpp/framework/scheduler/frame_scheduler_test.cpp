#include "realmmesh/scheduler/frame_scheduler.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace realm::scheduler {
namespace {

class FakeFrameClock final : public IFrameClock {
public:
    [[nodiscard]] TimePoint now() const noexcept override { return now_; }

    void sleep_until(TimePoint deadline) override {
        deadlines_.push_back(deadline);
        now_ = deadline;
    }

    /// 模拟帧回调本身的耗时。
    void advance(std::chrono::nanoseconds duration) { now_ += duration; }

    [[nodiscard]] const std::vector<TimePoint>& deadlines() const noexcept {
        return deadlines_;
    }

private:
    TimePoint now_{};
    std::vector<TimePoint> deadlines_;
};

TEST(FrameSchedulerTest, RejectsZeroTickRate) {
    FakeFrameClock clock;

    EXPECT_THROW(FrameScheduler(0, clock), std::invalid_argument);
}

TEST(FrameSchedulerTest, AdvancesFramesUsingFixedDeadlines) {
    using namespace std::chrono_literals;

    FakeFrameClock clock;
    FrameScheduler scheduler(20, clock);
    std::vector<FrameContext> frames;

    const auto completed_frames = scheduler.run([&frames](FrameContext frame) {
        frames.push_back(frame);
        return frame.index < 3;
    });

    ASSERT_EQ(frames.size(), 3U);
    EXPECT_EQ(completed_frames, 3U);
    EXPECT_EQ(frames[0].index, 1U);
    EXPECT_EQ(frames[1].index, 2U);
    EXPECT_EQ(frames[2].index, 3U);
    EXPECT_EQ(frames[0].delta, 50ms);

    ASSERT_EQ(clock.deadlines().size(), 2U);
    EXPECT_EQ(clock.deadlines()[0].time_since_epoch(), 50ms);
    EXPECT_EQ(clock.deadlines()[1].time_since_epoch(), 100ms);
}

TEST(FrameSchedulerTest, ReportsWorkDurationAndOverrunPerFrame) {
    using namespace std::chrono_literals;

    FakeFrameClock clock;
    FrameScheduler scheduler(20, clock);
    std::vector<FrameTiming> timings;
    scheduler.set_frame_observer(
        [&timings](const FrameTiming& timing) { timings.push_back(timing); });

    static_cast<void>(scheduler.run([&clock](FrameContext frame) {
        clock.advance(frame.index == 2 ? 60ms : 3ms);
        return frame.index < 3;
    }));

    // 末帧(回调返回 false)同样上报。
    ASSERT_EQ(timings.size(), 3U);
    EXPECT_EQ(timings[0].index, 1U);
    EXPECT_EQ(timings[0].work, 3ms);
    EXPECT_EQ(timings[0].budget, 50ms);
    EXPECT_FALSE(timings[0].overrun());
    EXPECT_EQ(timings[1].work, 60ms);
    EXPECT_TRUE(timings[1].overrun());
    EXPECT_EQ(timings[2].index, 3U);
    EXPECT_FALSE(timings[2].overrun());
}

}  // namespace
}  // namespace realm::scheduler
