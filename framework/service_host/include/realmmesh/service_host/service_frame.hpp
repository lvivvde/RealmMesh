#pragma once

#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/client_session_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace realm::cluster {
class ServiceResolver;
}  // namespace realm::cluster

namespace realm::game::gateway {
class GatewayRuntime;
}  // namespace realm::game::gateway

namespace realm::observability {
class Logger;
}  // namespace realm::observability

namespace realm::service_host {

/// 单服务业务帧:搬运自旧 apps/{login,realm,gateway}/main.cpp 的消息循环。
/// login/realm/gateway 之外的服务名不处理业务消息(帧循环空转);
/// 三类已知服务要求 REALMMESH_SESSION_TICKET_KEY 已设置(与旧 main 一致,
/// 缺失时构造抛 std::runtime_error)。
class ServiceFrame final {
public:
    /// downstream 为静态兜底下游(login→realm、realm→gateway;gateway 不用)。
    ServiceFrame(
        std::string_view service_name,
        std::string downstream_address,
        std::uint16_t downstream_port,
        std::size_t max_events_per_frame);

    /// service_started(gateway 另发每 transport 的 listener_started)。
    void started(
        observability::Logger& logger,
        const game::gateway::GatewayRuntime& runtime) const;
    /// 每帧业务:drain 事件并按服务名分发处理。
    void tick(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver);
    /// service_stopped(gateway 带 runtime 统计)。
    void stopped(
        observability::Logger& logger,
        const game::gateway::GatewayRuntime& runtime) const;

private:
    void handle_login_events(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver);
    void handle_realm_events(
        observability::Logger& logger,
        game::gateway::GatewayRuntime& runtime,
        cluster::ServiceResolver* resolver);
    void handle_gateway_events(
        observability::Logger& logger, game::gateway::GatewayRuntime& runtime);

    std::string service_name_;
    std::string downstream_address_;
    std::uint16_t downstream_port_{0};
    std::size_t max_events_per_frame_{0};
    game::common::SessionTicketCodec tickets_;
    game::common::TicketReplayGuard replay_guard_;
    std::unordered_map<
        game::gateway::ClientSessionId,
        game::common::SessionTicketClaims>
        authenticated_;
    std::unordered_map<std::string, game::common::SessionTicketClaims>
        pending_authenticated_;
};

}  // namespace realm::service_host
