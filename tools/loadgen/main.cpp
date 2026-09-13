#include "realmmesh/loadgen/loadgen.hpp"
#include "realmmesh/loadgen/robot.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

namespace {

using realm::loadgen::LoadgenConfig;
using realm::loadgen::Profile;
using realm::loadgen::RobotPhase;
using realm::loadgen::ServiceAddress;

void print_usage() {
    std::cout <<
        "realm_mesh_loadgen — RealmMesh 全链路机器人负载发生器\n"
        "\n"
        "用法: realm_mesh_loadgen --phase <p> [选项]\n"
        "\n"
        "  --phase verify|tickets|poll|gateway|all   机器人阶段\n"
        "      verify   登录验证一次\n"
        "      tickets  验证 + 取号\n"
        "      poll     验证 + 取号 + progress 轮询到放行兑换\n"
        "      gateway  单趟链路到 handed-off(1303)\n"
        "      all      循环链路到 --duration(soak 水位保持)\n"
        "  --robots N            机器人数(默认 1)\n"
        "  --ramp-seconds F      爬坡时长,按序错峰(默认 0)\n"
        "  --duration-seconds F  总时长/截止(默认 10)\n"
        "  --concurrency N       并发机器人上限(默认 32)\n"
        "  --poll-interval-ms N  progress 轮询间隔(默认 100)\n"
        "  --login-verify H:P    健全服地址\n"
        "  --queue H:P           排队服地址\n"
        "  --gateway H:P         网关地址\n"
        "  --metrics-endpoint H:P  结束后抓取 /metrics 进报告\n"
        "  --accounts N          账号数(机器人按 i%N 取号命名)\n"
        "  --account-prefix S    账号名前缀(默认 robot)\n"
        "  --credential S        登录凭据(默认 loadgen-credential)\n"
        "  --profile soak|m2     内置档:soak=10万机器人/30 分钟,\n"
        "                        m2=10万机器人/60s(显式参数覆盖)\n"
        "\n"
        "soak/m2 全量档建议专用大内存机器执行;CI 只跑缩减档。\n";
}

template <typename Integer>
[[nodiscard]] std::optional<Integer> parse_integer(std::string_view text) {
    Integer value{};
    const auto result = std::from_chars(
        text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<double> parse_double(std::string_view text) {
    // strtod 就地解析;要求整段消费且非负(时长语义)。
    const std::string buffer{text};
    char* end = nullptr;
    const double value = std::strtod(buffer.c_str(), &end);
    if (end != buffer.c_str() + buffer.size() || value < 0) {
        return std::nullopt;
    }
    return value;
}

[[nodiscard]] std::optional<ServiceAddress> parse_address(
    std::string_view text) {
    const auto colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0 ||
        colon + 1 == text.size()) {
        return std::nullopt;
    }
    const auto port = parse_integer<std::uint64_t>(text.substr(colon + 1));
    if (!port.has_value() || *port == 0 || *port > 65535) {
        return std::nullopt;
    }
    ServiceAddress address;
    address.host = std::string{text.substr(0, colon)};
    address.port = static_cast<std::uint16_t>(*port);
    return address;
}

}  // namespace

int main(int argc, char** argv) {
    using realm::loadgen::parse_robot_phase;

    LoadgenConfig config;
    bool phase_given = false;
    bool robots_given = false;
    bool ramp_given = false;
    bool duration_given = false;
    bool login_verify_given = false;
    bool queue_given = false;
    bool gateway_given = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument{argv[i]};
        const auto value_of = [&]() -> std::optional<std::string_view> {
            if (i + 1 >= argc) return std::nullopt;
            return std::string_view{argv[++i]};
        };

        if (argument == "--phase") {
            const auto value = value_of();
            const auto phase =
                value.has_value() ? parse_robot_phase(*value) : std::nullopt;
            if (!phase.has_value()) {
                std::cerr << "invalid --phase\n";
                return 2;
            }
            config.phase = *phase;
            phase_given = true;
        } else if (argument == "--profile") {
            const auto value = value_of();
            if (!value.has_value()) {
                std::cerr << "missing --profile value\n";
                return 2;
            }
            if (*value == "soak") {
                config.profile = Profile::Soak;
            } else if (*value == "m2") {
                config.profile = Profile::M2;
            } else {
                std::cerr << "invalid --profile (soak|m2)\n";
                return 2;
            }
        } else if (argument == "--robots") {
            const auto value = value_of();
            const auto robots =
                value.has_value() ? parse_integer<std::uint64_t>(*value)
                                  : std::nullopt;
            if (!robots.has_value()) {
                std::cerr << "invalid --robots\n";
                return 2;
            }
            config.robots = *robots;
            robots_given = true;
        } else if (argument == "--ramp-seconds") {
            const auto value = value_of();
            const auto seconds =
                value.has_value() ? parse_double(*value) : std::nullopt;
            if (!seconds.has_value()) {
                std::cerr << "invalid --ramp-seconds\n";
                return 2;
            }
            config.ramp_seconds = *seconds;
            ramp_given = true;
        } else if (argument == "--duration-seconds") {
            const auto value = value_of();
            const auto seconds =
                value.has_value() ? parse_double(*value) : std::nullopt;
            if (!seconds.has_value()) {
                std::cerr << "invalid --duration-seconds\n";
                return 2;
            }
            config.duration_seconds = *seconds;
            duration_given = true;
        } else if (argument == "--concurrency") {
            const auto value = value_of();
            const auto concurrency =
                value.has_value() ? parse_integer<std::uint64_t>(*value)
                                  : std::nullopt;
            if (!concurrency.has_value() || *concurrency == 0) {
                std::cerr << "invalid --concurrency\n";
                return 2;
            }
            config.concurrency = *concurrency;
        } else if (argument == "--poll-interval-ms") {
            const auto value = value_of();
            const auto milliseconds =
                value.has_value() ? parse_integer<std::int64_t>(*value)
                                  : std::nullopt;
            if (!milliseconds.has_value() || *milliseconds <= 0) {
                std::cerr << "invalid --poll-interval-ms\n";
                return 2;
            }
            config.poll_interval = std::chrono::milliseconds{*milliseconds};
        } else if (argument == "--login-verify") {
            const auto value = value_of();
            const auto address =
                value.has_value() ? parse_address(*value) : std::nullopt;
            if (!address.has_value()) {
                std::cerr << "invalid --login-verify (H:P)\n";
                return 2;
            }
            config.endpoints.login_verify = *address;
            login_verify_given = true;
        } else if (argument == "--queue") {
            const auto value = value_of();
            const auto address =
                value.has_value() ? parse_address(*value) : std::nullopt;
            if (!address.has_value()) {
                std::cerr << "invalid --queue (H:P)\n";
                return 2;
            }
            config.endpoints.queue = *address;
            queue_given = true;
        } else if (argument == "--gateway") {
            const auto value = value_of();
            const auto address =
                value.has_value() ? parse_address(*value) : std::nullopt;
            if (!address.has_value()) {
                std::cerr << "invalid --gateway (H:P)\n";
                return 2;
            }
            config.endpoints.gateway = *address;
            gateway_given = true;
        } else if (argument == "--metrics-endpoint") {
            const auto value = value_of();
            const auto address =
                value.has_value() ? parse_address(*value) : std::nullopt;
            if (!address.has_value()) {
                std::cerr << "invalid --metrics-endpoint (H:P)\n";
                return 2;
            }
            config.metrics_endpoint = *address;
        } else if (argument == "--accounts") {
            const auto value = value_of();
            const auto accounts =
                value.has_value() ? parse_integer<std::uint64_t>(*value)
                                  : std::nullopt;
            if (!accounts.has_value()) {
                std::cerr << "invalid --accounts\n";
                return 2;
            }
            config.accounts = *accounts;
        } else if (argument == "--account-prefix") {
            const auto value = value_of();
            if (!value.has_value()) {
                std::cerr << "missing --account-prefix value\n";
                return 2;
            }
            config.account_prefix = std::string{*value};
        } else if (argument == "--credential") {
            const auto value = value_of();
            if (!value.has_value()) {
                std::cerr << "missing --credential value\n";
                return 2;
            }
            config.credential = std::string{*value};
        } else if (argument == "--help") {
            print_usage();
            return 0;
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            return 2;
        }
    }

    // 内置档提供默认规模;显式参数覆盖(ADR-0003:每参数有消费路径)。
    if (config.profile == Profile::Soak) {
        if (!robots_given) config.robots = 100000;
        if (!ramp_given) config.ramp_seconds = 300;
        if (!duration_given) config.duration_seconds = 1800;
        if (!phase_given) config.phase = RobotPhase::All;
    } else if (config.profile == Profile::M2) {
        if (!robots_given) config.robots = 100000;
        if (!ramp_given) config.ramp_seconds = 30;
        if (!duration_given) config.duration_seconds = 60;
        if (!phase_given) config.phase = RobotPhase::Gateway;
    }

    if (!phase_given) {
        std::cerr << "--phase is required\n";
        return 2;
    }
    if (!login_verify_given) {
        std::cerr << "--login-verify is required\n";
        return 2;
    }
    if (config.phase != RobotPhase::Verify && !queue_given) {
        std::cerr << "--queue is required\n";
        return 2;
    }
    if ((config.phase == RobotPhase::Gateway ||
         config.phase == RobotPhase::All) &&
        !gateway_given) {
        std::cerr << "--gateway is required\n";
        return 2;
    }
    if (config.robots == 0) {
        std::cerr << "--robots must be positive\n";
        return 2;
    }

    const auto report = realm::loadgen::run_loadgen(config);
    std::cout << report.render();

    return report.completed == report.robots ? 0 : 1;
}
