#include "realmmesh/concurrency/bounded_work_pool.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

namespace realm::concurrency {
namespace {

using namespace std::chrono_literals;

/// 测试内的手动闸门:任务阻塞直到 open()。
class Gate {
public:
    void wait() {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return open_; });
    }
    void open() {
        {
            const std::scoped_lock lock(mutex_);
            open_ = true;
        }
        cv_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cv_;
    bool open_{false};
};

template <typename Result>
std::vector<typename BoundedWorkPool<Result>::Completion> drain_until(
    BoundedWorkPool<Result>& pool,
    std::size_t expected,
    std::chrono::milliseconds timeout = 2s) {
    std::vector<typename BoundedWorkPool<Result>::Completion> all;
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (all.size() < expected && std::chrono::steady_clock::now() < deadline) {
        auto batch = pool.drain(expected);
        for (auto& completion : batch) {
            all.push_back(std::move(completion));
        }
        if (all.size() < expected) {
            std::this_thread::sleep_for(1ms);
        }
    }
    return all;
}

TEST(BoundedWorkPoolTest, RejectsZeroWorkersOrCapacity) {
    EXPECT_THROW(BoundedWorkPool<int>(0, 4), std::invalid_argument);
    EXPECT_THROW(BoundedWorkPool<int>(1, 0), std::invalid_argument);
}

TEST(BoundedWorkPoolTest, DeliversResultsTaggedWithSubmissionId) {
    BoundedWorkPool<int> pool(2, 8);

    ASSERT_EQ(pool.try_submit(7, [] { return 70; }), WorkSubmitResult::Submitted);
    ASSERT_EQ(pool.try_submit(9, [] { return 90; }), WorkSubmitResult::Submitted);

    auto completions = drain_until(pool, 2);
    ASSERT_EQ(completions.size(), 2U);
    std::sort(completions.begin(), completions.end(),
        [](const auto& left, const auto& right) { return left.id < right.id; });
    EXPECT_EQ(completions[0].id, 7U);
    EXPECT_EQ(completions[0].result, 70);
    EXPECT_EQ(completions[1].id, 9U);
    EXPECT_EQ(completions[1].result, 90);
}

TEST(BoundedWorkPoolTest, RunsJobsConcurrentlyAcrossWorkers) {
    BoundedWorkPool<bool> pool(2, 2);
    std::mutex mutex;
    std::condition_variable cv;
    int arrived = 0;
    auto rendezvous = [&] {
        std::unique_lock lock(mutex);
        ++arrived;
        cv.notify_all();
        // 单工作线程时第二个任务永远进不来,等待超时返回 false。
        return cv.wait_for(lock, 2s, [&] { return arrived == 2; });
    };

    ASSERT_EQ(pool.try_submit(1, rendezvous), WorkSubmitResult::Submitted);
    ASSERT_EQ(pool.try_submit(2, rendezvous), WorkSubmitResult::Submitted);

    const auto completions = drain_until(pool, 2, 5s);
    ASSERT_EQ(completions.size(), 2U);
    EXPECT_TRUE(completions[0].result);
    EXPECT_TRUE(completions[1].result);
}

TEST(BoundedWorkPoolTest, CapacityCountsQueuedRunningAndUndrainedWork) {
    BoundedWorkPool<int> pool(1, 2);
    Gate gate;

    ASSERT_EQ(pool.try_submit(1, [&] { gate.wait(); return 1; }),
        WorkSubmitResult::Submitted);
    ASSERT_EQ(pool.try_submit(2, [] { return 2; }), WorkSubmitResult::Submitted);
    EXPECT_EQ(pool.try_submit(3, [] { return 3; }), WorkSubmitResult::Full);
    EXPECT_EQ(pool.in_flight(), 2U);

    gate.open();
    // 两个任务都跑完但结果尚未全部取走前,槽位不释放。
    const auto first = drain_until(pool, 1);
    ASSERT_EQ(first.size(), 1U);
    EXPECT_EQ(pool.in_flight(), 1U);

    const auto second = drain_until(pool, 1);
    ASSERT_EQ(second.size(), 1U);
    EXPECT_EQ(pool.in_flight(), 0U);
    EXPECT_EQ(pool.try_submit(3, [] { return 3; }), WorkSubmitResult::Submitted);
}

TEST(BoundedWorkPoolTest, CancelledQueuedJobNeverRunsAndFreesItsSlot) {
    BoundedWorkPool<int> pool(1, 2);
    Gate gate;
    std::atomic<bool> second_ran{false};

    ASSERT_EQ(pool.try_submit(1, [&] { gate.wait(); return 1; }),
        WorkSubmitResult::Submitted);
    ASSERT_EQ(pool.try_submit(2, [&] { second_ran = true; return 2; }),
        WorkSubmitResult::Submitted);

    pool.cancel(2);
    EXPECT_EQ(pool.in_flight(), 1U);

    gate.open();
    const auto completions = drain_until(pool, 1);
    ASSERT_EQ(completions.size(), 1U);
    EXPECT_EQ(completions[0].id, 1U);
    EXPECT_FALSE(second_ran.load());
}

TEST(BoundedWorkPoolTest, CancelledRunningJobHoldsSlotUntilItFinishesAndDropsResult) {
    BoundedWorkPool<int> pool(1, 1);
    Gate gate;
    std::atomic<bool> started{false};

    ASSERT_EQ(pool.try_submit(1, [&] { started = true; gate.wait(); return 1; }),
        WorkSubmitResult::Submitted);
    while (!started.load()) {
        std::this_thread::sleep_for(1ms);
    }

    pool.cancel(1);
    // 运行中的任务无法打断:槽位仍被占用,背压真实反映阻塞的工作线程。
    EXPECT_EQ(pool.try_submit(2, [] { return 2; }), WorkSubmitResult::Full);

    gate.open();
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (pool.in_flight() != 0 && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(pool.in_flight(), 0U);
    EXPECT_TRUE(pool.drain(8).empty());
}

TEST(BoundedWorkPoolTest, CancelDropsUndrainedResult) {
    BoundedWorkPool<int> pool(1, 1);
    ASSERT_EQ(pool.try_submit(1, [] { return 1; }), WorkSubmitResult::Submitted);
    // 给任务足够时间跑完、结果落入完成队列(仍占槽,未被取走)。
    std::this_thread::sleep_for(50ms);

    pool.cancel(1);
    const auto settle = std::chrono::steady_clock::now() + 2s;
    while (pool.in_flight() != 0 && std::chrono::steady_clock::now() < settle) {
        std::this_thread::sleep_for(1ms);
    }
    EXPECT_EQ(pool.in_flight(), 0U);
    EXPECT_TRUE(pool.drain(8).empty());
}

TEST(BoundedWorkPoolTest, StoppedPoolRejectsSubmissions) {
    BoundedWorkPool<int> pool(1, 4);
    pool.stop();
    EXPECT_EQ(pool.try_submit(1, [] { return 1; }), WorkSubmitResult::Stopped);
}

}  // namespace
}  // namespace realm::concurrency
