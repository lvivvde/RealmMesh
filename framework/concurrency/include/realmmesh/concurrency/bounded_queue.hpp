#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace realm::concurrency {

template <typename T>
class BoundedQueue final {
public:
    explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {
        if (capacity_ == 0) {
            throw std::invalid_argument("bounded queue capacity must be positive");
        }
    }

    BoundedQueue(const BoundedQueue&) = delete;
    BoundedQueue& operator=(const BoundedQueue&) = delete;

    [[nodiscard]] bool try_push(const T& value) {
        const std::scoped_lock lock(mutex_);
        if (queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(value);
        return true;
    }

    [[nodiscard]] bool try_push(T&& value) {
        const std::scoped_lock lock(mutex_);
        if (queue_.size() >= capacity_) {
            return false;
        }
        queue_.push_back(std::move(value));
        return true;
    }

    [[nodiscard]] std::optional<T> try_pop() {
        const std::scoped_lock lock(mutex_);
        if (queue_.empty()) {
            return std::nullopt;
        }
        T value = std::move(queue_.front());
        queue_.pop_front();
        return value;
    }

    [[nodiscard]] std::vector<T> drain(std::size_t max_items) {
        const std::scoped_lock lock(mutex_);
        const auto count = std::min(max_items, queue_.size());
        std::vector<T> values;
        values.reserve(count);
        for (std::size_t index = 0; index < count; ++index) {
            values.push_back(std::move(queue_.front()));
            queue_.pop_front();
        }
        return values;
    }

    [[nodiscard]] std::size_t size() const {
        const std::scoped_lock lock(mutex_);
        return queue_.size();
    }

    [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

private:
    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::deque<T> queue_;
};

}  // namespace realm::concurrency
