#pragma once

#include "realmmesh/game/gateway/edge_fetch.hpp"
#include "realmmesh/game/gateway/edge_session_table.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace realm::game::gateway {

/// 一次已结算的拉取尝试(#44):Succeeded → 调用方迁 handed-off
/// (拉取槽即还);Exhausted → 调用方断开会话(双预算随关闭路径归还)。
enum class EdgeFetchEventKind : std::uint8_t {
    Succeeded,
    Exhausted,
};

struct EdgeFetchEvent {
    EdgeSessionId session_id;
    EdgeFetchEventKind kind{EdgeFetchEventKind::Succeeded};
    /// 登记时携带的拉取账号:帧侧签发 handoff 直连凭证免查表(#45)。
    std::uint64_t account_id{0};
    /// 成功交付的这次尝试耗时(源自报);失败路径无意义。#47 观测缝,
    /// 帧侧据此累计 edge_fetch_duration_seconds。
    std::chrono::milliseconds attempt_duration{0};
};

/// 拉取调度器(spec #44 域内核):每 fetching 会话至多一次在途尝试;
/// 到期发起、按源自报耗时结算;失败按 基数×2^已耗重试数 退避,重试
/// 超过 retry_max 次上报 Exhausted。纯域无 IO:tick(now) 由帧线程驱动,
/// 结算事件由调用方按管线阶段幂等应用(未知/非 fetching 会话丢弃),
/// 调度器自身不持有会话表、不感知传输形态。
class EdgeFetchScheduler final {
public:
    /// retry_base:首次重试基数,此后每轮倍增;retry_max:失败后的
    /// 最大重试次数(不含首发,连同首发共 retry_max+1 次尝试)。
    EdgeFetchScheduler(
        std::chrono::milliseconds retry_base,
        unsigned retry_max,
        EdgeFetchSource& source);

    /// fetching 会话登记;首发 due = now。
    void register_session(
        EdgeSessionId session,
        std::uint64_t account_id,
        std::chrono::steady_clock::time_point now);

    /// 会话终结(对端断开/本地关闭):丢弃在途与待重试条目。
    void cancel(EdgeSessionId session);

    /// 驱动一次:到期发起 → 到点结算;返回本帧结算事件,每会话至多
    /// 一条;Succeeded/Exhausted 条目随即出表(结算即终局)。
    [[nodiscard]] std::vector<EdgeFetchEvent> tick(
        std::chrono::steady_clock::time_point now);

    /// 在途或待重试的拉取条目数(与 Edge Session 的 pending 阶段无关)。
    [[nodiscard]] std::size_t active() const noexcept {
        return attempts_.size();
    }

    /// 已发起的重试尝试累计(不含首发;单调,冷启动归零)。
    /// #47 帧尾轮询发布 edge_fetch_retry_total 用。
    [[nodiscard]] std::uint64_t retry_total() const noexcept {
        return retry_total_;
    }

private:
    struct Attempt {
        std::uint64_t account_id{0};
        unsigned attempts_made{0};
        std::chrono::steady_clock::time_point due{};
        bool in_flight{false};
        bool last_ok{false};
        std::chrono::milliseconds last_duration{0};
        std::chrono::steady_clock::time_point completes_at{};
    };

    std::chrono::milliseconds retry_base_;
    unsigned retry_max_;
    EdgeFetchSource* source_;
    std::unordered_map<EdgeSessionId, Attempt> attempts_;
    std::uint64_t retry_total_{0};
};

}  // namespace realm::game::gateway
