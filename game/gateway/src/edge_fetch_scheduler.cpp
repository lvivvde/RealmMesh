#include "realmmesh/game/gateway/edge_fetch_scheduler.hpp"

namespace realm::game::gateway {

EdgeFetchScheduler::EdgeFetchScheduler(
    std::chrono::milliseconds retry_base,
    unsigned retry_max,
    EdgeFetchSource& source)
    : retry_base_(retry_base),
      retry_max_(retry_max),
      source_(&source) {}

void EdgeFetchScheduler::register_session(
    EdgeSessionId session,
    std::uint64_t account_id,
    std::chrono::steady_clock::time_point now) {
    auto& attempt = attempts_[session];
    attempt.account_id = account_id;
    attempt.due = now;  // 首发:登记即到期
}

void EdgeFetchScheduler::cancel(EdgeSessionId session) {
    static_cast<void>(attempts_.erase(session));
}

std::vector<EdgeFetchEvent> EdgeFetchScheduler::tick(
    std::chrono::steady_clock::time_point now) {
    // 两段式:先统一发起(同一帧内到期的尝试彼此公平),再统一结算,
    // 使 0 耗时失败在同一帧内即可排定下一轮退避。
    for (auto& [session, attempt] : attempts_) {
        if (!attempt.in_flight && attempt.due <= now) {
            const auto outcome = source_->fetch(attempt.account_id);
            ++attempt.attempts_made;
            attempt.in_flight = true;
            attempt.last_ok = outcome.ok;
            attempt.completes_at = now + outcome.duration;
        }
    }

    std::vector<EdgeFetchEvent> events;
    std::vector<EdgeSessionId> finished;
    for (auto& [session, attempt] : attempts_) {
        if (!attempt.in_flight || attempt.completes_at > now) {
            continue;
        }
        attempt.in_flight = false;
        if (attempt.last_ok) {
            events.push_back(
                {session,
                 EdgeFetchEventKind::Succeeded,
                 attempt.account_id});
            finished.push_back(session);
            continue;
        }
        const unsigned retries_done = attempt.attempts_made - 1;
        if (retries_done >= retry_max_) {
            events.push_back(
                {session,
                 EdgeFetchEventKind::Exhausted,
                 attempt.account_id});
            finished.push_back(session);
            continue;
        }
        attempt.due = now + retry_base_ * (1U << retries_done);
    }
    for (const auto session : finished) {
        static_cast<void>(attempts_.erase(session));
    }
    return events;
}

}  // namespace realm::game::gateway
