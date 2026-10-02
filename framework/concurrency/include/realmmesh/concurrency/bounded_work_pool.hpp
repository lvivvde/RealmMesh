#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <stop_token>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realm::concurrency {

enum class WorkSubmitResult : std::uint8_t {
    Submitted,
    Full,
    Stopped,
};

/// 固定 N 个工作线程执行阻塞任务,结果经 drain() 交回单一属主线程。
/// 容量统计「排队 + 运行中 + 已完成未取走」,因此属主线程先查 in_flight()
/// 再提交不会与工作线程竞争出超额。任务不得抛异常(调用方自行包装)。
/// 同一时刻 id 须唯一,由调用方保证。
template <typename Result>
class BoundedWorkPool final {
public:
    struct Completion {
        std::uint64_t id{0};
        Result result;
    };

    using Job = std::function<Result()>;

    BoundedWorkPool(std::size_t workers, std::size_t capacity)
        : capacity_(capacity) {
        if (workers == 0) {
            throw std::invalid_argument("work pool needs at least one worker");
        }
        if (capacity == 0) {
            throw std::invalid_argument("work pool capacity must be positive");
        }
        workers_.reserve(workers);
        for (std::size_t index = 0; index < workers; ++index) {
            workers_.emplace_back([this](std::stop_token stop) { run(stop); });
        }
    }

    ~BoundedWorkPool() { stop(); }

    BoundedWorkPool(const BoundedWorkPool&) = delete;
    BoundedWorkPool& operator=(const BoundedWorkPool&) = delete;

    [[nodiscard]] WorkSubmitResult try_submit(std::uint64_t id, Job job) {
        {
            const std::scoped_lock lock(mutex_);
            if (stopped_) {
                return WorkSubmitResult::Stopped;
            }
            if (in_flight_ >= capacity_) {
                return WorkSubmitResult::Full;
            }
            queue_.push_back(Queued{.id = id, .job = std::move(job)});
            ++in_flight_;
        }
        cv_.notify_one();
        return WorkSubmitResult::Submitted;
    }

    [[nodiscard]] std::vector<Completion> drain(std::size_t max_items) {
        const std::scoped_lock lock(mutex_);
        const auto count = std::min(max_items, completions_.size());
        std::vector<Completion> drained;
        drained.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            drained.push_back(std::move(completions_.front()));
            completions_.pop_front();
        }
        in_flight_ -= count;
        return drained;
    }

    /// 排队中的任务直接丢弃;已完成未取走的结果直接丢弃;运行中的任务
    /// 无法打断,跑完后丢弃结果、释放槽位。未知 id 忽略。
    void cancel(std::uint64_t id) {
        const std::scoped_lock lock(mutex_);
        const auto queued = std::find_if(queue_.begin(), queue_.end(),
            [id](const Queued& entry) { return entry.id == id; });
        if (queued != queue_.end()) {
            queue_.erase(queued);
            --in_flight_;
            return;
        }
        const auto completed = std::find_if(
            completions_.begin(), completions_.end(),
            [id](const Completion& entry) { return entry.id == id; });
        if (completed != completions_.end()) {
            completions_.erase(completed);
            --in_flight_;
            return;
        }
        if (running_.contains(id)) {
            cancelled_.insert(id);
        }
    }

    [[nodiscard]] std::size_t in_flight() const {
        const std::scoped_lock lock(mutex_);
        return in_flight_;
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    /// 丢弃排队任务,等运行中的任务结束后回收全部工作线程。幂等。
    void stop() {
        {
            const std::scoped_lock lock(mutex_);
            stopped_ = true;
            in_flight_ -= queue_.size();
            queue_.clear();
        }
        for (auto& worker : workers_) {
            worker.request_stop();
        }
        cv_.notify_all();
        workers_.clear();
    }

private:
    struct Queued {
        std::uint64_t id{0};
        Job job;
    };

    void run(std::stop_token stop) {
        while (true) {
            Queued next;
            {
                std::unique_lock lock(mutex_);
                if (!cv_.wait(lock, stop, [this] { return !queue_.empty(); })) {
                    return;
                }
                next = std::move(queue_.front());
                queue_.pop_front();
                running_.insert(next.id);
            }
            auto result = next.job();
            const std::scoped_lock lock(mutex_);
            running_.erase(next.id);
            if (cancelled_.erase(next.id) > 0) {
                --in_flight_;
                continue;
            }
            completions_.push_back(
                Completion{.id = next.id, .result = std::move(result)});
        }
    }

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable_any cv_;
    std::deque<Queued> queue_;
    std::deque<Completion> completions_;
    std::unordered_set<std::uint64_t> running_;
    std::unordered_set<std::uint64_t> cancelled_;
    std::size_t in_flight_{0};
    bool stopped_{false};
    std::vector<std::jthread> workers_;
};

}  // namespace realm::concurrency
