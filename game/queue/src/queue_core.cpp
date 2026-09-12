#include "realmmesh/game/queue/queue_core.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace realm::game::queue {

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
    std::size_t idempotency_capacity)
    : release_step_(release_step),
      rate_window_(rate_window),
      idempotency_ttl_(idempotency_ttl),
      idempotency_capacity_(idempotency_capacity) {
    if (rate_window_ <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("rate window must be positive");
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
    for (const auto& record : releases_) {
        if (record.at <= cutoff) {
            continue;
        }
        total += record.amount;
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
        static_cast<void>(
            released_number_.fetch_add(batch, std::memory_order_relaxed));
        releases_.push_back({now, batch});
    }
    prune_releases(now);
    return batch;
}

void QueueCore::restore(const QueueSnapshot& snapshot) {
    if (snapshot.released_number >= snapshot.next_number) {
        throw std::invalid_argument("inconsistent queue snapshot");
    }
    released_number_.store(snapshot.released_number, std::memory_order_relaxed);
    next_number_.store(snapshot.next_number, std::memory_order_relaxed);
    releases_.clear();  // 实测速率随实例重启重建,不跨快照延续
}

QueueSnapshot QueueCore::snapshot(
    std::chrono::system_clock::time_point now) const {
    return {released_number_.load(std::memory_order_relaxed),
            next_number_.load(std::memory_order_relaxed),
            admit_rate(now)};
}

void QueueCore::prune_releases(std::chrono::system_clock::time_point now) {
    const auto cutoff = now - rate_window_;
    while (!releases_.empty() && releases_.front().at <= cutoff) {
        releases_.pop_front();
    }
}

}  // namespace realm::game::queue
