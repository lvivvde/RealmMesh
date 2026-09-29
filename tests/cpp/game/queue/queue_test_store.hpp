#pragma once

#include "realmmesh/game/queue/queue_store.hpp"

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realm::game::queue {

/// QueueStateStore 的确定性测试适配器。它在公共存取缝上模拟原子
/// 发号，不暴露 QueueCore 或生产 etcd 实现细节。
class TestQueueStateStore final : public QueueStateStore {
public:
    void set_issue_available(bool value) {
        const std::scoped_lock lock(mutex_);
        issue_available_ = value;
    }

    void set_budgets(std::optional<BudgetAggregate> value) {
        const std::scoped_lock lock(mutex_);
        budgets_ = std::move(value);
    }

    void set_snapshot(std::optional<QueueSnapshot> value) {
        const std::scoped_lock lock(mutex_);
        snapshot_ = std::move(value);
    }

    void set_save_success(bool value) {
        const std::scoped_lock lock(mutex_);
        save_success_ = value;
    }

    [[nodiscard]] std::size_t save_attempts() const {
        const std::scoped_lock lock(mutex_);
        return save_attempts_;
    }

    [[nodiscard]] std::vector<QueueSnapshot> saved() const {
        const std::scoped_lock lock(mutex_);
        return saved_;
    }

    [[nodiscard]] std::optional<BudgetAggregate> refresh_budgets()
        const override {
        const std::scoped_lock lock(mutex_);
        return budgets_;
    }

    [[nodiscard]] std::optional<QueueSnapshot> load_snapshot() const override {
        const std::scoped_lock lock(mutex_);
        return snapshot_;
    }

    [[nodiscard]] QueueIssueResult issue_or_recover(
        const QueueIssueRequest& request) const override {
        const std::scoped_lock lock(mutex_);
        if (!issue_available_) return {};
        const auto found = issues_.find(request.identity_jti);
        if (found != issues_.end()) {
            return {
                .status = QueueIssueStatus::Recovered,
                .number = found->second.number,
                .issued_at = found->second.issued_at,
                .snapshot = snapshot_.value_or(request.snapshot),
            };
        }
        auto committed = request.snapshot;
        const auto number = committed.next_number++;
        issues_.emplace(
            request.identity_jti,
            StoredIssue{
                number, request.issued_at, request.identity_expires_at});
        snapshot_ = committed;
        return {
            .status = QueueIssueStatus::Issued,
            .number = number,
            .issued_at = request.issued_at,
            .snapshot = std::move(committed),
        };
    }

    [[nodiscard]] bool save_snapshot(
        const QueueSnapshot& snapshot_value,
        std::int64_t updated_at_seconds) const override {
        const std::scoped_lock lock(mutex_);
        static_cast<void>(updated_at_seconds);
        ++save_attempts_;
        if (!save_success_) return false;
        snapshot_ = snapshot_value;
        saved_.push_back(snapshot_value);
        return true;
    }

private:
    struct StoredIssue final {
        std::uint64_t number;
        std::chrono::system_clock::time_point issued_at;
        std::chrono::system_clock::time_point identity_expires_at;
    };

    mutable std::mutex mutex_;
    mutable std::unordered_map<std::string, StoredIssue> issues_;
    std::optional<BudgetAggregate> budgets_;
    mutable std::optional<QueueSnapshot> snapshot_;
    mutable std::vector<QueueSnapshot> saved_;
    mutable std::size_t save_attempts_{0};
    bool issue_available_{true};
    bool save_success_{true};
};

}  // namespace realm::game::queue
