#pragma once
// Scene 编排的实现细节：协程只在同一个编排线程恢复，IO/数据库线程只完成
// future。
#include <coroutine>
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <utility>
#include <vector>
#include "realmmesh/concurrency/bounded_work_pool.hpp"

namespace realm::game::realm::scene_detail {
template <class T>
struct TaskValue {
    std::optional<T> value;
    void return_value(T result) { value = std::move(result); }
    T take() { return std::move(*value); }
};
template <>
struct TaskValue<void> {
    void return_void() {}
    void take() {}
};
template <class T = void>
class Task {
public:
    struct promise_type : TaskValue<T> {
        std::exception_ptr error;
        std::coroutine_handle<> continuation{std::noop_coroutine()};
        Task get_return_object() {
            return Task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct Final {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                return h.promise().continuation;
            }
            void await_resume() noexcept {}
        };
        Final final_suspend() noexcept { return {}; }
        void unhandled_exception() { error = std::current_exception(); }
    };
    using Handle = std::coroutine_handle<promise_type>;
    explicit Task(Handle h)
        : handle_(h) {}
    Task(Task&& other) noexcept
        : handle_(std::exchange(other.handle_, {})) {}
    Task& operator=(Task&& other) noexcept {
        if (this == &other) return *this;
        if (handle_) handle_.destroy();
        handle_ = std::exchange(other.handle_, {});
        return *this;
    }
    ~Task() {
        if (handle_) handle_.destroy();
    }
    Task(const Task&) = delete;
    bool done() const { return handle_.done(); }
    void start() { handle_.resume(); }
    bool await_ready() const { return false; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> parent) {
        handle_.promise().continuation = parent;
        return handle_;
    }
    T await_resume() {
        if (handle_.promise().error)
            std::rethrow_exception(handle_.promise().error);
        return handle_.promise().take();
    }

private:
    Handle handle_;
};

class Scheduler {
    struct Waiting {
        std::function<bool()> ready;
        std::coroutine_handle<> handle;
    };

public:
    template <class T>
    struct Future {
        Scheduler& scheduler;
        std::shared_ptr<std::future<T>> value;
        bool await_ready() const {
            return value->wait_for(std::chrono::seconds(0)) ==
                   std::future_status::ready;
        }
        void await_suspend(std::coroutine_handle<> h) {
            scheduler.waiting_.push_back(
                {[v = value] {
                     return v->wait_for(std::chrono::seconds(0)) ==
                            std::future_status::ready;
                 },
                 h});
        }
        T await_resume() { return value->get(); }
    };
    template <class T>
    Future<T> wait(std::future<T> value) {
        return {*this, std::make_shared<std::future<T>>(std::move(value))};
    }
    void spawn(Task<> task) {
        roots_.push_back(std::move(task));
        roots_.back().start();
    }
    void poll() {
        auto batch = std::move(waiting_);
        waiting_.clear();
        for (auto& entry : batch) {
            if (entry.ready())
                entry.handle.resume();
            else
                waiting_.push_back(std::move(entry));
        }
        std::erase_if(roots_, [](const auto& task) {
            return task.done();
        });
    }
    bool idle() const { return roots_.empty(); }

private:
    std::vector<Waiting> waiting_;
    std::vector<Task<>> roots_;
};

class DatabaseJobs {
public:
    template <class F>
    auto submit(F action) {
        using Result = std::invoke_result_t<F>;
        auto promise = std::make_shared<std::promise<Result>>();
        auto future = promise->get_future();
        const auto submitted =
            pool_.try_submit(next_id_++, [promise, action = std::move(action)] {
                try {
                    promise->set_value(action());
                } catch (...) {
                    promise->set_exception(std::current_exception());
                }
                return true;
            });
        if (submitted != concurrency::WorkSubmitResult::Submitted)
            promise->set_exception(
                std::make_exception_ptr(
                    std::runtime_error("scene database queue full")));
        return future;
    }
    void drain() { static_cast<void>(pool_.drain(256)); }

private:
    concurrency::BoundedWorkPool<bool> pool_{4, 256};
    std::uint64_t next_id_{1};
};
}  // namespace realm::game::realm::scene_detail
