#pragma once

#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/game/gateway/edge_session_table.hpp"

#include <chrono>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace realm::game::gateway {

struct AccountFetchAttemptId {
    std::uint64_t value{0};
    auto operator<=>(const AccountFetchAttemptId&) const = default;
};

struct AccountFetchRequest {
    AccountFetchAttemptId attempt_id;
    EdgeSessionId session_id;
    std::uint64_t account_id{0};
};

struct AccountFetchCompletion {
    AccountFetchAttemptId attempt_id;
    bool ok{false};
    std::chrono::milliseconds duration{0};
    enum class Status : std::uint8_t {
        Succeeded,
        NotEligible,
        Unavailable,
    } status{Status::Unavailable};
    std::uint64_t character_id{0};
    std::uint32_t realm_id{0};
    std::uint64_t character_revision{0};
};

using AccountFetchStatus = AccountFetchCompletion::Status;

enum class AccountFetchSubmitResult : std::uint8_t {
    Submitted,
    Full,
    Stopped,
};

class AccountFetchPort {
public:
    virtual ~AccountFetchPort() = default;
    [[nodiscard]] virtual AccountFetchSubmitResult submit(
        AccountFetchRequest request,
        std::chrono::steady_clock::time_point now) = 0;
    [[nodiscard]] virtual std::vector<AccountFetchCompletion>
    drain_completions(
        std::chrono::steady_clock::time_point now,
        std::size_t max_completions) = 0;
    virtual void cancel(AccountFetchAttemptId attempt_id) = 0;
};

/// 当前固定延迟行为的非阻塞过渡适配器。submit 只登记到期时间。
class DelayedAccountFetchPort final : public AccountFetchPort {
public:
    explicit DelayedAccountFetchPort(
        std::chrono::milliseconds latency = std::chrono::milliseconds{100},
        std::size_t capacity = 1'000);

    [[nodiscard]] AccountFetchSubmitResult submit(
        AccountFetchRequest request,
        std::chrono::steady_clock::time_point now) override;
    [[nodiscard]] std::vector<AccountFetchCompletion> drain_completions(
        std::chrono::steady_clock::time_point now,
        std::size_t max_completions) override;
    void cancel(AccountFetchAttemptId attempt_id) override;
    void stop() noexcept { stopped_ = true; }

private:
    struct Pending {
        std::chrono::steady_clock::time_point due;
    };

    std::chrono::milliseconds latency_;
    std::size_t capacity_;
    bool stopped_{false};
    std::unordered_map<std::uint64_t, Pending> pending_;
};

/// 权威玩家数据(生产为 MongoPlayerDataStore)的有界异步适配器。查询在
/// workers 个工作线程上并发执行；submit 不做网络 I/O，容量(排队 + 运行中 +
/// 未取走)耗尽与存储查询失败分别映射为 Full 和 Unavailable completion。
/// 单次查询的超时由 Pipeline 按 fetch_timeout 判定并 cancel。
class PlayerDataAccountFetchPort final : public AccountFetchPort {
public:
    PlayerDataAccountFetchPort(
        std::unique_ptr<const common::PlayerDataReader> reader,
        std::size_t capacity,
        std::size_t workers);
    ~PlayerDataAccountFetchPort();

    PlayerDataAccountFetchPort(const PlayerDataAccountFetchPort&) = delete;
    PlayerDataAccountFetchPort& operator=(const PlayerDataAccountFetchPort&) =
        delete;

    [[nodiscard]] AccountFetchSubmitResult submit(
        AccountFetchRequest request,
        std::chrono::steady_clock::time_point now) override;
    [[nodiscard]] std::vector<AccountFetchCompletion> drain_completions(
        std::chrono::steady_clock::time_point now,
        std::size_t max_completions) override;
    void cancel(AccountFetchAttemptId attempt_id) override;
    void stop() noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

class ScriptedAccountFetchPort final : public AccountFetchPort {
public:
    void script_submit_results(std::deque<AccountFetchSubmitResult> results);
    void push_completion(AccountFetchCompletion completion);
    void stop() noexcept { stopped_ = true; }

    [[nodiscard]] const std::vector<AccountFetchRequest>& submitted_requests()
        const noexcept {
        return submitted_;
    }
    [[nodiscard]] bool was_cancelled(AccountFetchAttemptId attempt_id) const;

    [[nodiscard]] AccountFetchSubmitResult submit(
        AccountFetchRequest request,
        std::chrono::steady_clock::time_point now) override;
    [[nodiscard]] std::vector<AccountFetchCompletion> drain_completions(
        std::chrono::steady_clock::time_point now,
        std::size_t max_completions) override;
    void cancel(AccountFetchAttemptId attempt_id) override;

private:
    bool stopped_{false};
    std::deque<AccountFetchSubmitResult> submit_results_;
    std::deque<AccountFetchCompletion> completions_;
    std::vector<AccountFetchRequest> submitted_;
    std::unordered_set<std::uint64_t> cancelled_;
};

}  // namespace realm::game::gateway
