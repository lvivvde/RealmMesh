#pragma once

#include "realmmesh/game/gateway/account_fetch_port.hpp"
#include "realmmesh/game/gateway/gateway_login_config.hpp"
#include "realmmesh/game/gateway/gateway_primary_transport.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

namespace realm::observability {
class Logger;
class MetricsRegistry;
}  // namespace realm::observability

namespace realm::game::gateway {

class GatewayAdmission;

enum class GatewayPipelineHealth : std::uint8_t {
    Healthy,
    Unhealthy,
};

struct GatewayAdmissionBudget {
    std::uint64_t conn_free{0};
    std::uint64_t fetch_free{0};
    bool available{false};
};

struct GatewayLoginFrame {
    std::chrono::steady_clock::time_point now;
    std::chrono::system_clock::time_point verification_now;
    std::optional<RealmEndpoint> discovered_realm;
};

struct GatewayLoginAdvanceResult {
    GatewayPipelineHealth health{GatewayPipelineHealth::Healthy};
    GatewayAdmissionBudget local_budget;
};

/// Gateway 内部登录编排的唯一外部接口。所有阶段、凭据提交、拉取重试、
/// Runtime 意图、Handoff、预算与指标状态均封装在一次 advance() 内。
class GatewayLoginPipeline final {
public:
    [[nodiscard]] static GatewayLoginPipeline create(
        GatewayLoginConfig config,
        GatewaySigningMaterial signing_material,
        GatewayPrimaryTransport& primary_transport,
        AccountFetchPort& account_fetch,
        observability::Logger* logger = nullptr,
        observability::MetricsRegistry* metrics = nullptr);

    /// #82 切换前的 Admission Grant 装配入口。外部仍只有 advance()；
    /// GatewayAdmission 在 Pipeline 内部吞掉 reserve/commit/release 顺序。
    /// 最终切换会删除上面的 admitted Queue Number 装配入口。
    [[nodiscard]] static GatewayLoginPipeline create(
        GatewayLoginConfig config,
        common::SessionTicketKey enter_realm_key,
        GatewayAdmission& admission,
        std::string gateway_instance,
        GatewayPrimaryTransport& primary_transport,
        AccountFetchPort& account_fetch,
        observability::Logger* logger = nullptr,
        observability::MetricsRegistry* metrics = nullptr);

    ~GatewayLoginPipeline();
    GatewayLoginPipeline(GatewayLoginPipeline&&) noexcept;
    GatewayLoginPipeline& operator=(GatewayLoginPipeline&&) noexcept;
    GatewayLoginPipeline(const GatewayLoginPipeline&) = delete;
    GatewayLoginPipeline& operator=(const GatewayLoginPipeline&) = delete;

    [[nodiscard]] GatewayLoginAdvanceResult advance(GatewayLoginFrame frame);

private:
    class Impl;
    explicit GatewayLoginPipeline(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::game::gateway
