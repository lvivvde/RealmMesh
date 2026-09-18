#include "realmmesh/game/gateway/account_fetch_port.hpp"

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
