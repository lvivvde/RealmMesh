#pragma once

#include "realmmesh/client/login_chain.hpp"
#include "realmmesh/client/wire_login_transport.hpp"
#include "realmmesh/loadgen/stats.hpp"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace realm::loadgen {

enum class LoadgenLoginTarget {
    Verify,
    Tickets,
    Poll,
    Gateway,
    GatewaySoak,
    Full,
};

/// CLI 兼容拼法 `all` 在解析边界立即收敛为 GatewaySoak。
[[nodiscard]] std::optional<LoadgenLoginTarget> parse_loadgen_login_target(
    std::string_view text);

enum class LoadgenPollingProfile {
    ClientRealistic,
    Pressure,
};

struct LoadgenEndpoints final {
    ServiceAddress login_verify;
    ServiceAddress queue;
    ServiceAddress gateway;
};

struct LoadgenLoginOptions final {
    LoadgenLoginTarget target{LoadgenLoginTarget::GatewaySoak};
    LoadgenPollingProfile polling{LoadgenPollingProfile::Pressure};
    LoadgenEndpoints endpoints;
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
/// 行为由 target 枚举决定；hold_until 只参与 GatewaySoak 校验。
[[nodiscard]] LoginRunAdaptation adapt_login_run(
    const LoadgenLoginOptions& options);

}  // namespace realm::loadgen
