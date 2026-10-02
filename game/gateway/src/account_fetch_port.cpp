#include "realmmesh/game/gateway/account_fetch_port.hpp"

#include "realmmesh/game/common/player_data_store.hpp"

#include "realmmesh/concurrency/bounded_work_pool.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace realm::game::gateway {

DelayedAccountFetchPort::DelayedAccountFetchPort(
    std::chrono::milliseconds latency, std::size_t capacity)
    : latency_(latency), capacity_(capacity) {
    if (latency_ < std::chrono::milliseconds::zero() || capacity_ == 0) {
        throw std::invalid_argument(
            "account fetch latency must be nonnegative and capacity positive");
    }
}

AccountFetchSubmitResult DelayedAccountFetchPort::submit(
    AccountFetchRequest request, std::chrono::steady_clock::time_point now) {
    if (stopped_) return AccountFetchSubmitResult::Stopped;
    if (pending_.size() >= capacity_) return AccountFetchSubmitResult::Full;
    if (request.attempt_id.value == 0 ||
        pending_.contains(request.attempt_id.value)) {
        return AccountFetchSubmitResult::Full;
    }
    pending_.emplace(request.attempt_id.value, Pending{now + latency_});
    return AccountFetchSubmitResult::Submitted;
}

std::vector<AccountFetchCompletion>
DelayedAccountFetchPort::drain_completions(
    std::chrono::steady_clock::time_point now, std::size_t max_completions) {
    std::vector<AccountFetchCompletion> completions;
    completions.reserve(std::min(max_completions, pending_.size()));
    for (auto entry = pending_.begin();
         entry != pending_.end() && completions.size() < max_completions;) {
        if (entry->second.due > now) {
            ++entry;
            continue;
        }
        completions.push_back(
            {AccountFetchAttemptId{entry->first}, true, latency_});
        entry = pending_.erase(entry);
    }
    return completions;
}

void DelayedAccountFetchPort::cancel(AccountFetchAttemptId attempt_id) {
    static_cast<void>(pending_.erase(attempt_id.value));
}

class PlayerDataAccountFetchPort::Impl final {
public:
    Impl(
        std::unique_ptr<const common::PlayerDataReader> reader,
        std::size_t capacity,
        std::size_t workers)
        : reader_(validated(std::move(reader))), pool_(workers, capacity) {}

    [[nodiscard]] AccountFetchSubmitResult submit(AccountFetchRequest request) {
        if (request.attempt_id.value == 0) {
            return AccountFetchSubmitResult::Full;
        }
        const auto submitted = pool_.try_submit(
            request.attempt_id.value,
            [this, request] { return fetch(request); });
        switch (submitted) {
        case concurrency::WorkSubmitResult::Submitted:
            return AccountFetchSubmitResult::Submitted;
        case concurrency::WorkSubmitResult::Full:
            return AccountFetchSubmitResult::Full;
        case concurrency::WorkSubmitResult::Stopped:
            return AccountFetchSubmitResult::Stopped;
        }
        return AccountFetchSubmitResult::Stopped;
    }

    [[nodiscard]] std::vector<AccountFetchCompletion> drain(
        std::size_t max_completions) {
        auto finished = pool_.drain(max_completions);
        std::vector<AccountFetchCompletion> drained;
        drained.reserve(finished.size());
        for (auto& completion : finished) {
            drained.push_back(std::move(completion.result));
        }
        return drained;
    }

    void cancel(AccountFetchAttemptId attempt_id) {
        pool_.cancel(attempt_id.value);
    }

    void stop() noexcept { pool_.stop(); }

private:
    [[nodiscard]] static std::unique_ptr<const common::PlayerDataReader>
    validated(std::unique_ptr<const common::PlayerDataReader> reader) {
        if (reader == nullptr) {
            throw std::invalid_argument("account fetch reader must not be null");
        }
        return reader;
    }

    /// 只在工作线程执行;存储故障映射为 Unavailable,不外抛。
    [[nodiscard]] AccountFetchCompletion fetch(
        const AccountFetchRequest& request) const noexcept {
        const auto started = std::chrono::steady_clock::now();
        AccountFetchCompletion completion;
        completion.attempt_id = request.attempt_id;
        try {
            const auto facts = reader_->login_facts(request.account_id);
            completion.ok = facts.has_value();
            completion.status = facts.has_value()
                                    ? AccountFetchStatus::Succeeded
                                    : AccountFetchStatus::NotEligible;
        } catch (...) {
            completion.ok = false;
            completion.status = AccountFetchStatus::Unavailable;
        }
        completion.duration =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started);
        return completion;
    }

    std::unique_ptr<const common::PlayerDataReader> reader_;
    // 最后声明:析构先回收工作线程,再释放它们引用的 reader_。
    concurrency::BoundedWorkPool<AccountFetchCompletion> pool_;
};

PlayerDataAccountFetchPort::PlayerDataAccountFetchPort(
    std::unique_ptr<const common::PlayerDataReader> reader,
    std::size_t capacity,
    std::size_t workers)
    : impl_(std::make_unique<Impl>(std::move(reader), capacity, workers)) {}

PlayerDataAccountFetchPort::~PlayerDataAccountFetchPort() = default;

AccountFetchSubmitResult PlayerDataAccountFetchPort::submit(
    AccountFetchRequest request, std::chrono::steady_clock::time_point) {
    return impl_->submit(std::move(request));
}

std::vector<AccountFetchCompletion> PlayerDataAccountFetchPort::drain_completions(
    std::chrono::steady_clock::time_point, std::size_t max_completions) {
    return impl_->drain(max_completions);
}

void PlayerDataAccountFetchPort::cancel(AccountFetchAttemptId attempt_id) {
    impl_->cancel(attempt_id);
}

void PlayerDataAccountFetchPort::stop() noexcept { impl_->stop(); }

void ScriptedAccountFetchPort::script_submit_results(
    std::deque<AccountFetchSubmitResult> results) {
    submit_results_ = std::move(results);
}

void ScriptedAccountFetchPort::push_completion(
    AccountFetchCompletion completion) {
    completions_.push_back(std::move(completion));
}

bool ScriptedAccountFetchPort::was_cancelled(
    AccountFetchAttemptId attempt_id) const {
    return cancelled_.contains(attempt_id.value);
}

AccountFetchSubmitResult ScriptedAccountFetchPort::submit(
    AccountFetchRequest request, std::chrono::steady_clock::time_point) {
    if (stopped_) return AccountFetchSubmitResult::Stopped;
    const auto result = submit_results_.empty()
                            ? AccountFetchSubmitResult::Submitted
                            : submit_results_.front();
    if (!submit_results_.empty()) submit_results_.pop_front();
    if (result == AccountFetchSubmitResult::Stopped) {
        stopped_ = true;
        return result;
    }
    if (result == AccountFetchSubmitResult::Submitted) {
        submitted_.push_back(std::move(request));
    }
    return result;
}

std::vector<AccountFetchCompletion>
ScriptedAccountFetchPort::drain_completions(
    std::chrono::steady_clock::time_point, std::size_t max_completions) {
    const auto count = std::min(max_completions, completions_.size());
    std::vector<AccountFetchCompletion> drained;
    drained.reserve(count);
    for (std::size_t position = 0; position < count; ++position) {
        drained.push_back(std::move(completions_.front()));
        completions_.pop_front();
    }
    return drained;
}

void ScriptedAccountFetchPort::cancel(AccountFetchAttemptId attempt_id) {
    cancelled_.insert(attempt_id.value);
}

}  // namespace realm::game::gateway
