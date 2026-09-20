#include "realmmesh/game/queue/queue_core.hpp"

#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/compact_jws.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace realm::game::queue {

void QueueSnapshot::validate() const {
    if (released_number >= next_number ||
        release_batches_pruned_through > released_number) {
        throw std::invalid_argument("inconsistent queue snapshot watermarks");
    }
    std::uint64_t expected = release_batches_pruned_through + 1U;
    std::chrono::system_clock::time_point previous_time{};
    bool first = true;
    for (const auto& batch : release_batches) {
        if (batch.first_number != expected ||
            batch.last_number < batch.first_number ||
            batch.last_number > released_number ||
            (!first && batch.released_at < previous_time)) {
            throw std::invalid_argument("inconsistent queue release ledger");
        }
        expected = batch.last_number + 1U;
        previous_time = batch.released_at;
        first = false;
    }
    if (expected != released_number + 1U) {
        throw std::invalid_argument("queue release ledger does not cover watermark");
    }
}

BudgetAggregate aggregate_budgets(
    std::span<const GatewayBudget> gateways,
    std::span<const RealmBudget> realms) {
    BudgetAggregate aggregate;
    for (const auto& gateway : gateways) {
        aggregate.gateway_admission +=
            std::min(gateway.conn_free, gateway.fetch_free);
    }
    for (const auto& realm : realms) {
        aggregate.realm_connections += realm.conn_free;
    }
    return aggregate;
}

QueueCore::QueueCore(
    std::uint64_t release_step,
    std::chrono::seconds rate_window,
    std::chrono::seconds idempotency_ttl,
    std::size_t idempotency_capacity,
    std::chrono::seconds grant_window)
    : release_step_(release_step),
      rate_window_(rate_window),
      idempotency_ttl_(idempotency_ttl),
      idempotency_capacity_(idempotency_capacity),
      grant_window_(grant_window) {
    if (rate_window_ <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("rate window must be positive");
    }
    if (grant_window_ <= std::chrono::seconds::zero() ||
        grant_window_ > common::admission_grant_max_window) {
        throw std::invalid_argument("grant window is outside protocol bounds");
    }
}

QueueCore::Issued QueueCore::issue(
    std::string identity_jti,
    std::chrono::system_clock::time_point now) {
    if (idempotency_.size() >= idempotency_capacity_) {
        const auto cutoff = now - idempotency_ttl_;
        std::erase_if(idempotency_, [cutoff](const auto& entry) {
            return entry.second.issued_at < cutoff;
        });
    }
    const auto number = next_number_.load(std::memory_order_relaxed);
    const auto [iterator, inserted] = idempotency_.try_emplace(
        std::move(identity_jti),
        IdempotencyEntry{number, now});
    if (inserted) {
        next_number_.store(number + 1, std::memory_order_relaxed);
        return {number, true};
    }
    iterator->second.issued_at = now;  // 幂等重放刷新时间戳
    return {iterator->second.number, false};
}

std::uint64_t QueueCore::released_number() const noexcept {
    return released_number_.load(std::memory_order_relaxed);
}

std::uint64_t QueueCore::next_number() const noexcept {
    return next_number_.load(std::memory_order_relaxed);
}

std::uint64_t QueueCore::admit_rate(
    std::chrono::system_clock::time_point now) const noexcept {
    const auto cutoff = now - rate_window_;
    std::uint64_t total = 0;
    for (const auto& batch : release_batches_) {
        if (batch.released_at <= cutoff) {
            continue;
        }
        total += batch.last_number - batch.first_number + 1U;
    }
    return total / static_cast<std::uint64_t>(rate_window_.count());
}

std::uint64_t QueueCore::release_batch(
    const BudgetAggregate& budgets,
    std::chrono::system_clock::time_point now) {
    const std::uint64_t allowance = std::min(
        {budgets.gateway_admission, budgets.realm_connections, release_step_});
    const std::uint64_t issued_ahead =
        next_number_.load(std::memory_order_relaxed) - 1 -
        released_number_.load(std::memory_order_relaxed);
    const std::uint64_t batch = std::min(allowance, issued_ahead);
    if (batch != 0) {
        // 快照与 JWS 都以 Unix 秒承载时间；在事实产生处归一化，保证
        // 重启前后窗口边界和确定性签名完全一致，而不是恢复后被截短。
        const auto released_at = std::chrono::system_clock::time_point{
            std::chrono::duration_cast<std::chrono::seconds>(
                now.time_since_epoch())};
        if (!release_batches_.empty() &&
            released_at < release_batches_.back().released_at) {
            throw std::runtime_error(
                "system clock moved backwards across queue release batches");
        }
        const auto previous =
            released_number_.fetch_add(batch, std::memory_order_relaxed);
        release_batches_.push_back(QueueReleaseBatch{
            .first_number = previous + 1U,
            .last_number = previous + batch,
            .released_at = released_at,
        });
    }
    prune_release_batches(now);
    return batch;
}

QueueReleaseEligibility QueueCore::release_eligibility(
    std::uint64_t number,
    std::chrono::system_clock::time_point now) const noexcept {
    if (number == 0 ||
        number > released_number_.load(std::memory_order_relaxed)) {
        return {.status = QueueReleaseStatus::NotReleased};
    }
    if (number <= release_batches_pruned_through_) {
        return {.status = QueueReleaseStatus::Expired};
    }
    for (const auto& batch : release_batches_) {
        if (number < batch.first_number) break;
        if (number <= batch.last_number) {
            const auto status =
                now > batch.released_at + grant_window_ +
                          common::jws_clock_leeway
                    ? QueueReleaseStatus::Expired
                    : QueueReleaseStatus::Eligible;
            return {.status = status, .released_at = batch.released_at};
        }
    }
    return {.status = QueueReleaseStatus::Expired};
}

void QueueCore::restore(
    const QueueSnapshot& snapshot,
    std::chrono::system_clock::time_point now) {
    snapshot.validate();
    for (const auto& batch : snapshot.release_batches) {
        if (batch.released_at > now + common::jws_clock_leeway) {
            throw std::invalid_argument("queue snapshot contains a future release");
        }
    }
    released_number_.store(snapshot.released_number, std::memory_order_relaxed);
    next_number_.store(snapshot.next_number, std::memory_order_relaxed);
    release_batches_pruned_through_ =
        snapshot.release_batches_pruned_through;
    release_batches_.assign(
        snapshot.release_batches.begin(), snapshot.release_batches.end());
    prune_release_batches(now);
}

QueueSnapshot QueueCore::snapshot(
    std::chrono::system_clock::time_point now) const {
    return {
        .released_number = released_number_.load(std::memory_order_relaxed),
        .next_number = next_number_.load(std::memory_order_relaxed),
        .admit_rate = admit_rate(now),
        .release_batches_pruned_through =
            release_batches_pruned_through_,
        .release_batches = {release_batches_.begin(), release_batches_.end()},
    };
}

void QueueCore::prune_release_batches(
    std::chrono::system_clock::time_point now) {
    while (!release_batches_.empty() &&
           now > release_batches_.front().released_at + grant_window_ +
                     common::jws_clock_leeway) {
        release_batches_pruned_through_ =
            release_batches_.front().last_number;
        release_batches_.pop_front();
    }
}

}  // namespace realm::game::queue
