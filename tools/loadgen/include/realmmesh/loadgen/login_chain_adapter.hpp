#pragma once

#include "realmmesh/client/login_chain.hpp"
#include "realmmesh/client/wire_login_transport.hpp"
#include "realmmesh/loadgen/robot.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <variant>

namespace realm::loadgen {

/// Loadgen 的配置层目标。All 只在这个 Adapter 层作为兼容别名存在，
/// 不会进入 framework Login Chain Interface。
enum class LoadgenLoginTarget {
    Verify,
    Tickets,
    Poll,
    Gateway,
    GatewaySoak,
    Full,
    All,
};

enum class LoadgenPollingProfile {
    ClientRealistic,
    Pressure,
};

struct LoadgenLoginOptions final {
    LoadgenLoginTarget target{LoadgenLoginTarget::All};
    LoadgenPollingProfile polling{LoadgenPollingProfile::Pressure};
    RobotEndpoints endpoints;
    std::string account;
    std::string credential;
    std::chrono::milliseconds poll_interval{100};
    client::TimePoint deadline{client::TimePoint::max()};
    std::optional<client::TimePoint> hold_until;
};

enum class LoginRunConfigError {
    MissingSoakHorizon,
    UnexpectedSoakHorizon,
    SoakHorizonInPast,
    NonPositivePollInterval,
    MissingGatewayEndpoint,
};

struct AdaptedLoginRun final {
    client::LoginRun run;
    client::LoginChainConfig chain;
    client::WireEndpoints wire;
    client::WireTransportOptions transport;
};

using LoginRunAdaptation =
    std::variant<AdaptedLoginRun, LoginRunConfigError>;

/// 把 loadgen 配置一次性收敛为合法 LoginRun 与生产 Adapter 配置。
/// 行为由 target 枚举决定；hold_until 只参与 GatewaySoak/All 校验。
[[nodiscard]] LoginRunAdaptation adapt_login_run(
    const LoadgenLoginOptions& options);

}  // namespace realm::loadgen
