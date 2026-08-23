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

}  // namespace
}  // namespace realm::scheduler
