#include "realmmesh/game/realm/realm_frame.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/edge/edge_runtime.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/observability/logger.hpp"

#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>

namespace realm::game::realm {
namespace {

[[nodiscard]] common::SessionTicketKey load_ticket_key() {
    const char* value = std::getenv("REALMMESH_SESSION_TICKET_KEY");
    if (value == nullptr) {
        throw std::runtime_error("REALMMESH_SESSION_TICKET_KEY is not set");
    }
    return common::parse_ticket_key_hex(value);
}

/// 入场拒绝的统一出口:未建立会话 decline,已建立会话回包后 close。
void reject_enter_realm(
    edge::EdgeRuntime& runtime,
    edge::EdgeConnectionId session_id,
    bool established,
    std::uint64_t request_id,
    int code,
    std::string_view message) {
    common::EdgeError error;
    error.set_code(static_cast<std::uint32_t>(code));
    error.set_message(std::string{message});
    const auto response = common::encode(error, request_id);
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
    edge::EdgeConnectionId session_id,
    const common::SessionTicketClaims& claims) {
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

RealmFrame::RealmFrame(
    std::size_t max_events_per_frame,
    std::uint64_t conn_capacity,
    RealmSessions& sessions)
    : max_events_per_frame_(max_events_per_frame),
      tickets_(load_ticket_key()),
      sessions_(&sessions),
      conn_capacity_(conn_capacity) {}

RealmFrame::~RealmFrame() = default;

bool RealmFrame::absorb_lifecycle(const edge::EdgeEvent& event) {
    // 连接计数先于其余分支:ConnectionOpened/ConnectionClosed 不产生业务
    // 事件,只增减活动连接数(#46 conn_free 快照的容量基准)。
    switch (event.kind) {
    case edge::EdgeEventKind::ConnectionOpened:
        ++conn_active_;
        return false;
    case edge::EdgeEventKind::ConnectionClosed:
        if (conn_active_ > 0) {
            --conn_active_;
        }
        authenticated_.erase(event.connection_id);
        sessions_->closed(event.connection_id);
        return false;
    default:
        return event.kind == edge::EdgeEventKind::MessageReceived;
    }
}

void RealmFrame::started(
    observability::Logger& logger,
    const edge::EdgeRuntime& runtime) const {
    static_cast<void>(logger.info(
        "service_started",
        "realm service started",
        {observability::field("listen_address", "0.0.0.0"),
         observability::field("listen_port", runtime.local_port())}));
}

void RealmFrame::stopped(observability::Logger& logger) const {
    static_cast<void>(
        logger.info("service_stopped", "realm service stopped"));
}

void RealmFrame::tick(
    observability::Logger& logger,
    edge::EdgeRuntime& runtime,
    cluster::InstanceBudgetReporter* budget_reporter) {
    for (auto& event : runtime.drain_events(max_events_per_frame_)) {
        if (!absorb_lifecycle(event)) {
            continue;
        }

        // 直连入场(#46):1304 是 1303 下发票据的消费端,客户端连接
        // 后即刻提交,此时会话通常仍在 pending 阶段。
        if (const auto request = common::decode_enter_realm(event.payload);
            request.has_value()) {
            handle_enter_realm(logger, runtime, event, *request);
            continue;
        }

        const auto request_id =
            common::edge_request_id(event.payload).value_or(0);
        const auto session = event.connection_id;
        // 未建立会话只受理 1304 直连入场(#46):其余消息一律按未认证
        // 拒绝。退役编号在解码层已不可识别,同样落到这里。
        if (!event.established) {
            common::EdgeError error;
            error.set_code(common::edge_error_not_authenticated);
            error.set_message("enter realm before any other message");
            static_cast<void>(runtime.try_decline(
                session, common::encode(error, request_id)));
            continue;
        }
        const bool authenticated = authenticated_.contains(session);
        // 被顶替的会话已收到 1409、关闭在途:关闭事件到达前的来包直接丢弃。
        if (authenticated && !sessions_->contains(session)) {
            continue;
        }
        if (authenticated &&
            common::decode_heartbeat_request(event.payload).has_value()) {
            common::HeartbeatResponse heartbeat;
            static_cast<void>(runtime.try_send(
                session, common::encode(heartbeat, request_id)));
            continue;
        }
        // 选角与训练(1401/1403/1405/1407)交给 RealmSessions;阶段校验、
        // 数据访问与回包都在那里。
        if (authenticated && sessions_->handle(session, event.payload)) {
            continue;
        }
        // 已建立会话:未认证的回 2002 后终结;已认证但不认识的消息回 2002
        // 并保持会话。
        common::EdgeError error;
        error.set_code(common::edge_error_not_authenticated);
        error.set_message("unsupported message");
        static_cast<void>(
            runtime.try_send(session, common::encode(error, request_id)));
        if (!authenticated) {
            static_cast<void>(runtime.try_close(session));
        }
    }
    sessions_->complete(max_events_per_frame_);
    // 帧尾发布额度快照(#46):Realm 仅 conn_free(has_fetch=false),
    // 写失败不更新已发布状态,后续帧自动重试。
    if (budget_reporter != nullptr) {
        const auto conn_free =
            conn_capacity_ > conn_active_ ? conn_capacity_ - conn_active_ : 0;
        static_cast<void>(budget_reporter->publish(
            cluster::InstanceBudgetSnapshot{conn_free, 0, false}));
    }
}

void RealmFrame::handle_enter_realm(
    observability::Logger& logger,
    edge::EdgeRuntime& runtime,
    const edge::EdgeEvent& event,
    const common::EnterRealm& request) {
    const auto request_id = common::edge_request_id(event.payload).value_or(0);
    if (authenticated_.contains(event.connection_id)) {
        // 已入场会话重复提交:协议违例,终结会话(不兑换新票据)。
        reject_enter_realm(
            runtime,
            event.connection_id,
            event.established,
            request_id,
            common::edge_error_invalid_enter_realm_ticket,
            "invalid enter realm ticket");
        return;
    }
    const auto redeemed = tickets_.redeem(
        common::protobuf_bytes(request.enter_realm_ticket()),
        common::TicketPurpose::EnterRealm);
    // 回放保护在 redeem 处烧票。票据只绑定账号与 Realm(ADR-0013),兑换
    // 不再读数据库;角色归属在选择角色时核对。
    const bool ticket_valid =
        redeemed.status == common::RedeemStatus::Accepted;
    if (!ticket_valid || redeemed.claims.realm_id != RealmSessions::realm_id) {
        reject_enter_realm(
            runtime,
            event.connection_id,
            event.established,
            request_id,
            common::edge_error_invalid_enter_realm_ticket,
            "invalid enter realm ticket");
        if (ticket_valid) {
            log_claim_mismatch(logger, event.connection_id, redeemed.claims);
        } else {
            static_cast<void>(logger.warn(
                "realm_enter_rejected",
                "enter realm ticket rejected",
                {observability::field(
                     "session_id",
                     event.connection_id.value,
                     observability::DataClass::Internal),
                 observability::field(
                     "redeem_status", static_cast<int>(redeemed.status))}));
        }
        return;
    }

    authenticated_[event.connection_id] = redeemed.claims;
    common::EnterRealmAccepted accepted;
    accepted.set_account_id(redeemed.claims.account_id);
    const auto response = common::encode(accepted, request_id);
    if (event.established) {
        static_cast<void>(runtime.try_send(event.connection_id, response));
    } else {
        static_cast<void>(runtime.try_accept(event.connection_id, response));
    }
    // 受理回包先于登记:同账号旧会话随后收到 1409 并被关闭。
    sessions_->enter(event.connection_id, redeemed.claims.account_id);
    static_cast<void>(logger.info(
        "player_session_established",
        "realm accepted direct entry",
        {observability::field(
            "account_id",
            redeemed.claims.account_id,
            observability::DataClass::Pseudonymous)},
        observability::EventContext{.request_id = request_id}));
}

}  // namespace realm::game::realm
