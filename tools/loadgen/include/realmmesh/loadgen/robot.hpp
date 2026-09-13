#pragma once

#include "realmmesh/loadgen/stats.hpp"

#include <chrono>
#include <optional>
#include <string>

namespace realm::loadgen {

/// 机器人跑到的阶段(spec --phase):verify / tickets / poll / gateway
/// / all。poll = progress 轮询 + 放行兑换(到 admit grant 为止);
/// gateway = 全链路到 handed-off(1303);all = gateway + 保持连接到
/// 总截止(soak 语境)。
enum class RobotPhase {
    Verify,
    Tickets,
    Poll,
    Gateway,
    All,
};

[[nodiscard]] std::optional<RobotPhase> parse_robot_phase(std::string_view text);

struct RobotEndpoints final {
    ServiceAddress login_verify;
    ServiceAddress queue;
    ServiceAddress gateway;
};

struct RobotOptions final {
    RobotPhase phase{RobotPhase::All};
    RobotEndpoints endpoints;
    /// 登录账号(须在账号表内且未封禁、在白名单)。
    std::string account;
    std::string credential;
    /// progress 轮询间隔。
    std::chrono::milliseconds poll_interval{100};
    /// 总截止:超时未走完链路 → RobotTimeout。
    std::chrono::steady_clock::time_point deadline{
        std::chrono::steady_clock::time_point::max()};
    /// all 阶段:到 handed-off 后保持连接至此(soak 水位)。
    std::optional<std::chrono::steady_clock::time_point> hold_until;
    /// 工件采集(测试断言用):true 时把取号得到的 queue_number_token
    /// 带回结果;规模档(soak/m2)保持 false,免百万级字符串驻留。
    bool collect_artifacts{false};
};

/// 单机器人单次执行结果:完成与否 + 终态失败分型。时延进调用方的
/// 相位计数器(复用同一 LoadgenReport 计数器,机器人串行打点)。
struct RobotOutcome final {
    bool completed{false};
    FailureKind failure{FailureKind::None};
    /// collect_artifacts 时填:取号相位的 queue_number_token(原样
    /// 返回,验签/claims 由调用方消费)。
    std::string number_token;
};

/// 驱动一个机器人走完其阶段的链路;相位计数写入 counters(verify/
/// tickets/poll/attach/handoff 五相位)。异常不外抛:网络层失败折算
/// 为 ConnectionError 计数。
[[nodiscard]] RobotOutcome run_robot(
    const RobotOptions& options,
    PhaseCounters& verify,
    PhaseCounters& tickets,
    PhaseCounters& poll,
    PhaseCounters& attach,
    PhaseCounters& handoff);

}  // namespace realm::loadgen
