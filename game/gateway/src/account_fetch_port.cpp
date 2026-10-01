#include "realmmesh/game/gateway/account_fetch_port.hpp"

#include "realmmesh/game/common/player_data_store.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>
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
        std::size_t capacity)
        : reader_(std::move(reader)), capacity_(capacity) {
        if (reader_ == nullptr) {
            throw std::invalid_argument("account fetch reader must not be null");
        }
        if (capacity_ == 0) {
            throw std::invalid_argument(
                "account fetch capacity must be positive");
        }
        // 参数校验通过后才启动工作线程，避免构造失败时线程已在运行。
        worker_ = std::jthread([this](std::stop_token token) { run(token); });
    }

    ~Impl() { stop(); }

    [[nodiscard]] AccountFetchSubmitResult submit(AccountFetchRequest request) {
        const std::scoped_lock lock(mutex_);
        if (stopped_) return AccountFetchSubmitResult::Stopped;
        if (request.attempt_id.value == 0 ||
            pending_ids_.contains(request.attempt_id.value) ||
            pending_ids_.size() >= capacity_) {
            return AccountFetchSubmitResult::Full;
        }
        pending_ids_.insert(request.attempt_id.value);
        requests_.push_back(std::move(request));
        condition_.notify_one();
        return AccountFetchSubmitResult::Submitted;
    }

    [[nodiscard]] std::vector<AccountFetchCompletion> drain(
        std::size_t max_completions) {
        const std::scoped_lock lock(mutex_);
        std::vector<AccountFetchCompletion> drained;
        drained.reserve(std::min(max_completions, completions_.size()));
        while (!completions_.empty() && drained.size() < max_completions) {
            auto completion = std::move(completions_.front());
            completions_.pop_front();
            pending_ids_.erase(completion.attempt_id.value);
            if (cancelled_.erase(completion.attempt_id.value) == 0) {
                drained.push_back(std::move(completion));
            }
        }
        return drained;
    }

    void cancel(AccountFetchAttemptId attempt_id) {
        const std::scoped_lock lock(mutex_);
        if (pending_ids_.contains(attempt_id.value)) {
            cancelled_.insert(attempt_id.value);
        }
        condition_.notify_all();
    }

    void stop() noexcept {
        {
            const std::scoped_lock lock(mutex_);
            if (stopped_) return;
            stopped_ = true;
            requests_.clear();
            completions_.clear();
            pending_ids_.clear();
            cancelled_.clear();
        }
        worker_.request_stop();
        condition_.notify_all();
    }

private:
    void run(std::stop_token token) noexcept {
        while (!token.stop_requested()) {
            AccountFetchRequest request;
            {
                std::unique_lock lock(mutex_);
                condition_.wait(lock, token, [this] {
                    return stopped_ || !requests_.empty();
                });
                if (stopped_ || token.stop_requested()) return;
                request = std::move(requests_.front());
                requests_.pop_front();
                if (cancelled_.contains(request.attempt_id.value)) {
                    pending_ids_.erase(request.attempt_id.value);
                    cancelled_.erase(request.attempt_id.value);
                    continue;
                }
            }

            const auto started = std::chrono::steady_clock::now();
            AccountFetchCompletion completion;
            completion.attempt_id = request.attempt_id;
            try {
                const auto facts = reader_->login_facts(request.account_id);
                completion.ok = facts.has_value();
                completion.status = facts.has_value()
                                        ? AccountFetchStatus::Succeeded
                                        : AccountFetchStatus::NotEligible;
                if (facts.has_value()) {
                    completion.character_id = facts->character_id;
                    completion.realm_id = facts->realm_id;
                    completion.character_revision =
                        facts->character_revision;
                }
            } catch (const common::PlayerDataError&) {
                completion.ok = false;
                completion.status = AccountFetchStatus::Unavailable;
            }
            completion.duration =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - started);

            const std::scoped_lock lock(mutex_);
            if (stopped_) return;
            if (cancelled_.contains(request.attempt_id.value)) {
                pending_ids_.erase(request.attempt_id.value);
                cancelled_.erase(request.attempt_id.value);
                continue;
            }
            completions_.push_back(std::move(completion));
        }
    }

    std::unique_ptr<const common::PlayerDataReader> reader_;
    std::size_t capacity_{0};
    std::mutex mutex_;
    std::condition_variable_any condition_;
    bool stopped_{false};
    std::deque<AccountFetchRequest> requests_;
    std::deque<AccountFetchCompletion> completions_;
    std::unordered_set<std::uint64_t> pending_ids_;
    std::unordered_set<std::uint64_t> cancelled_;
    std::jthread worker_;
};

PlayerDataAccountFetchPort::PlayerDataAccountFetchPort(
    std::unique_ptr<const common::PlayerDataReader> reader,
    std::size_t capacity)
    : impl_(std::make_unique<Impl>(std::move(reader), capacity)) {}

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
