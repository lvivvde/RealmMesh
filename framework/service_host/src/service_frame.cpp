#include "realmmesh/service_host/service_frame.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace realm::service_host {
namespace {

[[nodiscard]] game::common::SessionTicketKey load_ticket_key() {
    const char* value = std::getenv("REALMMESH_SESSION_TICKET_KEY");
    if (value == nullptr) {
        throw std::runtime_error("REALMMESH_SESSION_TICKET_KEY is not set");
    }
    return game::common::parse_ticket_key_hex(value);
}

/// 未知服务无业务帧,ticket 门面仅为成员占位(不会被调用);
/// codec 拒绝全零 key,故填非零哑值。
[[nodiscard]] game::common::SessionTicketKey service_ticket_key(
    const std::optional<cluster::ServiceType>& identity) {
    if (identity == cluster::ServiceType::Realm) {
        return load_ticket_key();
    }
    game::common::SessionTicketKey placeholder{};
    placeholder.fill(std::byte{1});
    return placeholder;
}

/// 入场拒绝的统一出口:未建立会话 decline,已建立会话回包后 close。
void reject_enter_realm(
    game::gateway::GatewayRuntime& runtime,
    game::gateway::EdgeSessionId session_id,
    bool established,
    std::uint64_t request_id,
    int code,
    std::string_view message,
    std::uint32_t retry_after_seconds = 0) {
    game::common::EdgeError error;
    error.set_code(static_cast<std::uint32_t>(code));
    error.set_message(std::string{message});
    if (retry_after_seconds > 0) {
        error.set_retry_after_seconds(retry_after_seconds);
    }
    const auto response = game::common::encode(error, request_id);
    if (established) {
        static_cast<void>(runtime.try_send(session_id, response));
        static_cast<void>(runtime.try_close(session_id));
    } else {
        static_cast<void>(runtime.try_decline(session_id, response));
    }
}

/// 票据验签通过、但 Realm 不符;此时才可安全记录账号标识(无效/重放时
/// redeem 不返回可信 claims)。
void log_claim_mismatch(
    observability::Logger& logger,
    game::gateway::EdgeSessionId session_id,
    const game::common::SessionTicketClaims& claims) {
    static_cast<void>(logger.warn(
        "realm_enter_claim_mismatch",
        "enter realm claims do not match this realm",
        {observability::field(
             "session_id",
             session_id.value,
             observability::DataClass::Internal),
         observability::field(
             "account_id",
             claims.account_id,
             observability::DataClass::Pseudonymous),
         observability::field("realm_id", claims.realm_id)}));
}

}  // namespace

std::optional<cluster::ServiceType> parse_service_identity(
    std::string_view service_name) {
    if (service_name == "gateway") return cluster::ServiceType::Gateway;
    if (service_name == "realm") return cluster::ServiceType::Realm;
    if (service_name == "login_verify")
        return cluster::ServiceType::LoginVerify;
    if (service_name == "queue") return cluster::ServiceType::Queue;
    return std::nullopt;
}

ServiceFrame::ServiceFrame(
    std::string_view service_name,
    std::string downstream_address,
    std::uint16_t downstream_port,
    std::size_t max_events_per_frame,
    std::uint64_t conn_capacity,
    game::gateway::GatewayLoginPipeline* gateway_login_pipeline,
    game::realm::RealmSessions* realm_sessions)
    : service_name_(service_name),
      max_events_per_frame_(max_events_per_frame),
      identity_(parse_service_identity(service_name)),
      tickets_(service_ticket_key(identity_)),
      gateway_login_pipeline_(gateway_login_pipeline),
      realm_sessions_(realm_sessions),
      conn_capacity_(conn_capacity) {
    if (identity_.has_value() && identity_ != cluster::ServiceType::Gateway &&
        (downstream_address.empty() || downstream_port == 0)) {
        throw std::invalid_argument(
            service_name_ + " downstream endpoint is required");
    }
    if (identity_ == cluster::ServiceType::Gateway &&
        gateway_login_pipeline_ == nullptr) {
        throw std::invalid_argument(
            "gateway login pipeline is required for gateway frame");
    }
    if (identity_ == cluster::ServiceType::Realm && realm_sessions_ == nullptr) {
        throw std::invalid_argument("realm sessions are required for realm frame");
    }
}

ServiceFrame::~ServiceFrame() = default;

bool ServiceFrame::ready() const noexcept {
    return gateway_ready_;
}

bool ServiceFrame::absorb_lifecycle(const game::gateway::GatewayEvent& event) {
    if (event.kind == game::gateway::GatewayEventKind::SessionClosed) {
        authenticated_.erase(event.session_id);
        if (realm_sessions_ != nullptr) {
            realm_sessions_->closed(event.session_id);
        }
        return false;
    }
    return event.kind == game::gateway::GatewayEventKind::MessageReceived;
}

void ServiceFrame::started(
    observability::Logger& logger,
    const game::gateway::GatewayRuntime& runtime) const {
    if (identity_ == cluster::ServiceType::Gateway) {
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
        return;
    }
    if (!identity_.has_value()) return;
    static_cast<void>(logger.info(
        "service_started",
        service_name_ + " service started",
        {observability::field("listen_address", "0.0.0.0"),
         observability::field("listen_port", runtime.local_port())}));
}

void ServiceFrame::tick(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    cluster::ServiceResolver* resolver,
    cluster::InstanceBudgetReporter* budget_reporter) {
    if (!identity_.has_value()) {
        return;
    }
    switch (*identity_) {
    case cluster::ServiceType::Realm:
        handle_realm_events(logger, runtime, budget_reporter);
        break;
    case cluster::ServiceType::Gateway:
        handle_gateway_events(logger, runtime, resolver, budget_reporter);
        break;
    case cluster::ServiceType::LoginVerify:
    case cluster::ServiceType::Queue:
        // HTTPS 请求循环形态不经 ServiceFrame(见 ServiceHost 装配),
        // 这里不可达;保持空转以穷尽枚举。
        break;
    }
}

void ServiceFrame::stopped(
    observability::Logger& logger,
    const game::gateway::GatewayRuntime& runtime) const {
    if (!identity_.has_value()) return;
    if (identity_ == cluster::ServiceType::Gateway) {
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
             observability::field(
                 "delivery_failed", stats.failed_deliveries),
             observability::field(
                 "invalid_source_disconnects",
                 stats.invalid_source_disconnects)}));
        return;
    }
    static_cast<void>(
        logger.info("service_stopped", service_name_ + " service stopped"));
}

void ServiceFrame::handle_realm_events(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    cluster::InstanceBudgetReporter* budget_reporter) {
    for (auto& event : runtime.drain_events(max_events_per_frame_)) {
        // 连接计数先于其余分支:SessionOpened/SessionClosed 不产生业务
        // 事件,只增减活动连接数(#46 conn_free 快照的容量基准)。
        if (event.kind == game::gateway::GatewayEventKind::SessionOpened) {
            ++realm_conn_active_;
        } else if (
            event.kind == game::gateway::GatewayEventKind::SessionClosed) {
            if (realm_conn_active_ > 0) {
                --realm_conn_active_;
            }
        }
        if (!absorb_lifecycle(event)) {
            continue;
        }

        // 直连入场(#46):1304 是 1303 下发票据的消费端,客户端连接
        // 后即刻提交,此时会话通常仍在 pending 阶段。
        if (const auto request =
                game::common::decode_enter_realm(event.payload);
            request.has_value()) {
            handle_enter_realm(logger, runtime, event, *request);
            continue;
        }

        const auto request_id =
            game::common::edge_request_id(event.payload).value_or(0);
        const auto session = event.session_id;
        // 未建立会话只受理 1304 直连入场(#46):其余消息一律按未认证
        // 拒绝。退役编号在解码层已不可识别,同样落到这里。
        if (!event.established) {
            game::common::EdgeError error;
            error.set_code(game::common::edge_error_not_authenticated);
            error.set_message("enter realm before any other message");
            static_cast<void>(runtime.try_decline(
                session, game::common::encode(error, request_id)));
            continue;
        }
        const bool authenticated = authenticated_.contains(session);
        // 被顶替的会话已收到 1409、关闭在途:关闭事件到达前的来包直接丢弃。
        if (authenticated && !realm_sessions_->contains(session)) {
            continue;
        }
        if (authenticated &&
            game::common::decode_heartbeat_request(event.payload).has_value()) {
            game::common::HeartbeatResponse heartbeat;
            static_cast<void>(runtime.try_send(
                session, game::common::encode(heartbeat, request_id)));
            continue;
        }
        // 选角与训练(1401/1403/1405/1407)交给 RealmSessions;阶段校验、
        // 数据访问与回包都在那里。
        if (authenticated && realm_sessions_->handle(session, event.payload)) {
            continue;
        }
        // 已建立会话:未认证的回 2002 后终结;已认证但不认识的消息回 2002
        // 并保持会话。
        game::common::EdgeError error;
        error.set_code(game::common::edge_error_not_authenticated);
        error.set_message("unsupported message");
        static_cast<void>(
            runtime.try_send(session, game::common::encode(error, request_id)));
        if (!authenticated) {
            static_cast<void>(runtime.try_close(session));
        }
    }
    realm_sessions_->complete(max_events_per_frame_);
    // 帧尾发布额度快照(#46):realm 仅 conn_free(has_fetch=false),
    // 策略节流在 InstanceBudgetReporter 内,写失败不更新已发布状态,
    // 后续帧自动重试。
    if (budget_reporter != nullptr) {
        const auto conn_free = conn_capacity_ > realm_conn_active_
                                   ? conn_capacity_ - realm_conn_active_
                                   : 0;
        static_cast<void>(budget_reporter->publish(
            cluster::InstanceBudgetSnapshot{conn_free, 0, false}));
    }
}

void ServiceFrame::handle_enter_realm(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    const game::gateway::GatewayEvent& event,
    const game::common::EnterRealm& request) {
    const auto request_id =
        game::common::edge_request_id(event.payload).value_or(0);
    if (authenticated_.contains(event.session_id)) {
        // 已入场会话重复提交:协议违例,终结会话(不兑换新票据)。
        reject_enter_realm(
            runtime,
            event.session_id,
            event.established,
            request_id,
            game::common::edge_error_invalid_enter_realm_ticket,
            "invalid enter realm ticket");
        return;
    }
    const auto redeemed = tickets_.redeem(
        game::common::protobuf_bytes(request.enter_realm_ticket()),
        game::common::TicketPurpose::EnterRealm);
    // 回放保护在 redeem 处烧票。票据只绑定账号与 Realm(ADR-0013),兑换
    // 不再读数据库;角色归属在选择角色时核对。
    const bool ticket_valid =
        redeemed.status == game::common::RedeemStatus::Accepted;
    if (!ticket_valid ||
        redeemed.claims.realm_id != game::realm::RealmSessions::realm_id) {
        reject_enter_realm(
            runtime,
            event.session_id,
            event.established,
            request_id,
            game::common::edge_error_invalid_enter_realm_ticket,
            "invalid enter realm ticket");
        if (ticket_valid) {
            log_claim_mismatch(logger, event.session_id, redeemed.claims);
        } else {
            static_cast<void>(logger.warn(
                "realm_enter_rejected",
                "enter realm ticket rejected",
                {observability::field(
                     "session_id",
                     event.session_id.value,
                     observability::DataClass::Internal),
                 observability::field(
                     "redeem_status", static_cast<int>(redeemed.status))}));
        }
        return;
    }

    authenticated_[event.session_id] = redeemed.claims;
    game::common::EnterRealmAccepted accepted;
    accepted.set_account_id(redeemed.claims.account_id);
    const auto response = game::common::encode(accepted, request_id);
    if (event.established) {
        static_cast<void>(runtime.try_send(event.session_id, response));
    } else {
        static_cast<void>(runtime.try_accept(event.session_id, response));
    }
    // 受理回包先于登记:同账号旧会话随后收到 1409 并被关闭。
    realm_sessions_->enter(event.session_id, redeemed.claims.account_id);
    static_cast<void>(logger.info(
        "player_session_established",
        "realm accepted direct entry",
        {observability::field(
            "account_id",
            redeemed.claims.account_id,
            observability::DataClass::Pseudonymous)},
        observability::EventContext{.request_id = request_id}));
}

void ServiceFrame::handle_gateway_events(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    cluster::ServiceResolver* resolver,
    cluster::InstanceBudgetReporter* budget_reporter) {
    std::optional<game::gateway::RealmEndpoint> discovered_realm;
    if (resolver != nullptr) {
        if (const auto endpoint = resolver->endpoint(); endpoint.has_value()) {
            discovered_realm =
                game::gateway::RealmEndpoint{endpoint->address, endpoint->port};
        }
    }

    const auto result = gateway_login_pipeline_->advance({
        .now = std::chrono::steady_clock::now(),
        .verification_now = std::chrono::system_clock::now(),
        .discovered_realm = std::move(discovered_realm),
    });
    gateway_ready_ =
        result.health == game::gateway::GatewayPipelineHealth::Healthy &&
        result.local_budget.available;
    if (budget_reporter != nullptr) {
        const auto& budget = result.local_budget;
        static_cast<void>(budget_reporter->publish(
            cluster::InstanceBudgetSnapshot{
                budget.available ? budget.conn_free : 0,
                budget.available ? budget.fetch_free : 0,
                true}));
    }
    if (result.health == game::gateway::GatewayPipelineHealth::Unhealthy) {
        static_cast<void>(logger.error(
            "gateway_login_pipeline_unhealthy",
            "gateway login pipeline dependency stopped; stopping gateway"));
        runtime.stop();
    }
}

}  // namespace realm::service_host
