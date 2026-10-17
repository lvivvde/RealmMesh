#include "realmmesh/game/gateway/gateway_frame.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/edge/edge_runtime.hpp"
#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <optional>
#include <utility>

namespace realm::game::gateway {

GatewayFrame::GatewayFrame(GatewayLoginPipeline& pipeline) noexcept
    : pipeline_(&pipeline) {}

bool GatewayFrame::ready() const noexcept {
    return ready_;
}

void GatewayFrame::started(
    observability::Logger& logger,
    const edge::EdgeRuntime& runtime) const {
    for (const auto& endpoint : runtime.local_endpoints()) {
        static_cast<void>(logger.info(
            "listener_started",
            "gateway listener started",
            {observability::field("listen_address", endpoint.address),
             observability::field("listen_port", endpoint.port),
             observability::field(
                 "transport", network::to_string(endpoint.protocol)),
             observability::field("transport_name", endpoint.name)}));
    }
    static_cast<void>(
        logger.info("service_started", "gateway service started"));
}

void GatewayFrame::tick(
    observability::Logger& logger,
    edge::EdgeRuntime& runtime,
    cluster::ServiceResolver* resolver,
    cluster::InstanceBudgetReporter* budget_reporter) {
    std::optional<RealmEndpoint> discovered_realm;
    if (resolver != nullptr) {
        if (const auto endpoint = resolver->endpoint(); endpoint.has_value()) {
            discovered_realm = RealmEndpoint{endpoint->address, endpoint->port};
        }
    }

    const auto result = pipeline_->advance({
        .now = std::chrono::steady_clock::now(),
        .verification_now = std::chrono::system_clock::now(),
        .discovered_realm = std::move(discovered_realm),
    });
    ready_ = result.health == GatewayPipelineHealth::Healthy &&
             result.local_budget.available;
    if (budget_reporter != nullptr) {
        const auto& budget = result.local_budget;
        static_cast<void>(budget_reporter->publish(
            cluster::InstanceBudgetSnapshot{
                budget.available ? budget.conn_free : 0,
                budget.available ? budget.fetch_free : 0,
                true}));
    }
    if (result.health == GatewayPipelineHealth::Unhealthy) {
        static_cast<void>(logger.error(
            "gateway_login_pipeline_unhealthy",
            "gateway login pipeline dependency stopped; stopping gateway"));
        runtime.stop();
    }
}

void GatewayFrame::stopped(
    observability::Logger& logger,
    const edge::EdgeRuntime& runtime) const {
    const auto stats = runtime.stats();
    static_cast<void>(logger.info(
        "service_stopped",
        "gateway service stopped",
        {observability::field(
             "overload_disconnects", stats.overload_disconnects),
         observability::field(
             "outbound_rejected", stats.rejected_outbound_commands),
         observability::field(
             "unknown_session_commands", stats.unknown_session_commands),
         observability::field("delivered", stats.successful_deliveries),
         observability::field("delivery_failed", stats.failed_deliveries),
         observability::field(
             "invalid_source_disconnects",
             stats.invalid_source_disconnects)}));
}

}  // namespace realm::game::gateway
