#pragma once

#include "realmmesh/loadgen/metrics_scrape.hpp"
#include "realmmesh/loadgen/robot.hpp"
#include "realmmesh/loadgen/stats.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace realm::loadgen {

/// 内置档(spec:soak = M1 参数化档,m2 = M2 等比档;CI 只跑缩减档,
/// 显式命令行参数覆盖档位默认)。
enum class Profile {
    None,
    Soak,
    M2,
};

struct LoadgenConfig final {
    RobotPhase phase{RobotPhase::All};
    Profile profile{Profile::None};
    std::uint64_t robots{1};
    double ramp_seconds{0};
    double duration_seconds{10};
    std::uint64_t concurrency{32};
    std::chrono::milliseconds poll_interval{100};
    RobotEndpoints endpoints;
    /// 账号命名:robot-<i % accounts>;账号表须先行备好同名记录。
    std::string account_prefix{"robot"};
    std::uint64_t accounts{0};
    std::string credential{"loadgen-credential"};
    /// 可选:结束后抓取该服务 /metrics 进报告(服务侧口径)。
    std::optional<ServiceAddress> metrics_endpoint;
    /// 工件采集(测试断言用):true 时把各机器人取到的号码牌汇进
    /// 报告(verify/tickets 相位有效);规模档保持 false。
    bool collect_number_tokens{false};
};

struct LoadgenReport final {
    std::uint64_t robots{0};
    std::uint64_t completed{0};
    /// 窗口关闭时还没起跑的机器人:不计链路成败,也不伪装成完成。
    std::uint64_t skipped{0};
    PhaseCounters verify;
    PhaseCounters tickets;
    PhaseCounters poll;
    PhaseCounters attach;
    PhaseCounters handoff;
    std::optional<MetricsSnapshot> service_metrics;
    /// collect_number_tokens 时非空:按完成顺序汇入的取号结果。
    std::vector<std::string> number_tokens;

    [[nodiscard]] std::string render() const;
};

/// 起跑 robots 个机器人(线程模型:线程数 = 并发槽数,机器人从原子
/// 游标依次领号;爬坡按序错峰)。阻塞至窗口关闭或全部结束。
[[nodiscard]] LoadgenReport run_loadgen(const LoadgenConfig& config);

}  // namespace realm::loadgen
