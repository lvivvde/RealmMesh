#include "realmmesh/service_host/service_frame.hpp"

#include "realmmesh/cluster/budget_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <cstdlib>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

namespace realm::service_host {
namespace {

/// 开发账号派生(login:credential == "dev" 即通过,账号 ID 为 FNV-1a)。
[[nodiscard]] std::uint64_t development_account_id(std::string_view account) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : account) {
        const auto value = static_cast<unsigned char>(character);
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

/// 开发角色派生(realm:每个账号固定一个英雄角色)。
[[nodiscard]] std::uint64_t development_character_id(std::uint64_t account_id) {
    return account_id ^ 0x524d434841524143ULL;
}

[[nodiscard]] game::common::SessionTicketKey load_ticket_key() {
    const char* value = std::getenv("REALMMESH_SESSION_TICKET_KEY");
    if (value == nullptr) {
        throw std::runtime_error("REALMMESH_SESSION_TICKET_KEY is not set");
    }
    return game::common::parse_ticket_key_hex(value);
}

/// 未知服务无业务帧,ticket 门面仅为成员占位(不会被调用);
/// codec 拒绝全零 key,故填非零哑值。
[[nodiscard]] game::common::SessionTicketKey make_ticket_key(
    const std::optional<cluster::ServiceType>& identity) {
    if (!identity.has_value()) {
        game::common::SessionTicketKey placeholder{};
        placeholder.fill(std::byte{1});
        return placeholder;
    }
    return load_ticket_key();
}

/// attach 验签 codec 的 kid:与签发方配置默认值一致(login_verify.kid =
/// login-verify-v1、queue.kid = queue-v1);JWKS 分发与 kid 轮换收敛后
/// 改为发现机制(与 queue 侧验签同源的过渡形态)。
constexpr std::string_view identity_token_kid = "login-verify-v1";
constexpr std::string_view queue_number_kid = "queue-v1";

/// attach 拒绝回包:尚未进入会话的事件(event.established 为假)经
/// try_decline 尽力回包并终结;已在管的会话回包后 try_close(状态机:
/// 凭据无效/额度外拒绝 → Closed)。
void decline_attach(
    game::gateway::GatewayRuntime& runtime,
    const game::gateway::GatewayEvent& event,
    std::uint64_t request_id,
    std::uint32_t code,
    std::string_view message) {
    game::common::EdgeError error;
    error.set_code(code);
    error.set_message(std::string(message));
    const auto response = game::common::encode(error, request_id);
    if (event.established) {
        static_cast<void>(runtime.try_send(event.session_id, response));
        static_cast<void>(runtime.try_close(event.session_id));
        return;
    }
    static_cast<void>(runtime.try_decline(event.session_id, response));
}

}  // namespace

std::optional<cluster::ServiceType> parse_service_identity(
    std::string_view service_name) {
    if (service_name == "gateway") return cluster::ServiceType::Gateway;
    if (service_name == "login") return cluster::ServiceType::Login;
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
    EdgePipelineCaps edge_pipeline_caps)
    : service_name_(service_name),
      downstream_address_(std::move(downstream_address)),
      downstream_port_(downstream_port),
      max_events_per_frame_(max_events_per_frame),
      identity_(parse_service_identity(service_name)),
      tickets_(make_ticket_key(identity_)) {
    if (identity_.has_value() &&
        identity_ != cluster::ServiceType::Gateway &&
        (downstream_address_.empty() || downstream_port_ == 0)) {
        throw std::invalid_argument(
            service_name_ + " downstream endpoint is required");
    }
    if (identity_ == cluster::ServiceType::Gateway) {
        pipeline_.emplace(
            edge_pipeline_caps.conn_capacity,
            edge_pipeline_caps.fetch_capacity);
    }
}

ServiceFrame::EdgeAttachContext::EdgeAttachContext(
    game::common::Ed25519Seed identity_seed,
    game::common::Ed25519Seed number_seed,
    game::gateway::EdgeSessionPipeline& pipeline,
    std::string_view identity_kid,
    std::string_view number_kid,
    std::string_view identity_issuer)
    : identity_codec(identity_seed, std::string(identity_kid)),
      number_codec(number_seed, std::string(number_kid)),
      chain(identity_codec, number_codec, pipeline, identity_issuer) {}

bool ServiceFrame::absorb_lifecycle(
    const game::gateway::GatewayEvent& event) {
    if (event.kind == game::gateway::GatewayEventKind::SessionClosed) {
        authenticated_.erase(event.session_id);
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
    case cluster::ServiceType::Login:
        handle_login_events(logger, runtime, resolver);
        break;
    case cluster::ServiceType::Realm:
        handle_realm_events(logger, runtime, resolver);
        break;
    case cluster::ServiceType::Gateway:
        handle_gateway_events(logger, runtime, budget_reporter);
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
                 "delivery_failed", stats.failed_deliveries)}));
        return;
    }
    static_cast<void>(
        logger.info("service_stopped", service_name_ + " service stopped"));
}

void ServiceFrame::handle_login_events(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    cluster::ServiceResolver* resolver) {
    for (auto& event : runtime.drain_events(max_events_per_frame_)) {
        if (!absorb_lifecycle(event)) {
            continue;
        }

        const auto request = game::common::decode_login_request(event.payload);
        const auto request_id =
            game::common::edge_request_id(event.payload).value_or(0);
        std::vector<std::byte> response;
        const bool authenticated = request.has_value() &&
                                   !request->account().empty() &&
                                   request->credential() == "dev";
        if (!authenticated) {
            game::common::EdgeError error;
            error.set_code(game::common::edge_error_invalid_credentials);
            error.set_message("invalid credentials");
            response = game::common::encode(error, request_id);
        } else {
            const auto account_id = development_account_id(request->account());
            const auto correlation_id = game::common::make_correlation_id();
            const auto correlation_text =
                game::common::correlation_id_hex(correlation_id);
            const auto discovered =
                resolver != nullptr ? resolver->endpoint() : std::nullopt;
            const auto realm_address = discovered.has_value()
                                           ? discovered->address
                                           : downstream_address_;
            const auto realm_port =
                discovered.has_value() ? discovered->port : downstream_port_;
            const auto ticket = tickets_.issue(
                game::common::TicketPurpose::Login,
                account_id,
                1,
                0,
                correlation_id,
                std::chrono::seconds(60));
            game::common::LoginSucceeded success;
            success.set_account_id(account_id);
            success.set_login_ticket(ticket.data(), ticket.size());
            auto* endpoint = success.add_realm_endpoints();
            endpoint->set_protocol(
                ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_TLS_TCP);
            endpoint->set_address(realm_address);
            endpoint->set_port(realm_port);
            endpoint->set_priority(0);
            response = game::common::encode(success, request_id);
            static_cast<void>(logger.info(
                "player_session_established",
                "login authenticated player session",
                {observability::field(
                    "account_id",
                    account_id,
                    observability::DataClass::Pseudonymous)},
                observability::EventContext{
                    .correlation_id = correlation_text,
                    .request_id = request_id,
                }));
        }
        if (event.established) {
            static_cast<void>(runtime.try_send(event.session_id, response));
        } else {
            static_cast<void>(runtime.try_accept(event.session_id, response));
        }
    }
}

void ServiceFrame::handle_realm_events(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    cluster::ServiceResolver* resolver) {
    for (auto& event : runtime.drain_events(max_events_per_frame_)) {
        if (!absorb_lifecycle(event)) {
            continue;
        }

        const auto request_id =
            game::common::edge_request_id(event.payload).value_or(0);
        std::vector<std::byte> response;
        if (!event.established) {
            const auto request =
                game::common::decode_realm_authenticate(event.payload);
            const auto redeemed = tickets_.redeem(
                request.has_value()
                    ? game::common::protobuf_bytes(request->login_ticket())
                    : std::span<const std::byte>{},
                game::common::TicketPurpose::Login);
            const bool authenticated =
                redeemed.status == game::common::RedeemStatus::Accepted &&
                redeemed.claims.realm_id == 1;
            if (!authenticated) {
                game::common::EdgeError error;
                error.set_code(game::common::edge_error_invalid_login_ticket);
                error.set_message("invalid login ticket");
                response = game::common::encode(error, request_id);
            } else {
                authenticated_[event.session_id] = redeemed.claims;
                game::common::CharacterList characters;
                auto* character = characters.add_characters();
                character->set_id(
                    development_character_id(redeemed.claims.account_id));
                character->set_name("Development Hero");
                response = game::common::encode(characters, request_id);
                observability::EventContext context{
                    .correlation_id = std::nullopt,
                    .request_id = request_id,
                };
                if (redeemed.claims.correlation_id.has_value()) {
                    context.correlation_id = game::common::correlation_id_hex(
                        *redeemed.claims.correlation_id);
                }
                static_cast<void>(logger.info(
                    "player_session_established",
                    "realm authenticated player session",
                    {observability::field(
                        "account_id",
                        redeemed.claims.account_id,
                        observability::DataClass::Pseudonymous)},
                    std::move(context)));
            }
            if (authenticated) {
                static_cast<void>(
                    runtime.try_accept(event.session_id, response));
            } else {
                static_cast<void>(
                    runtime.try_decline(event.session_id, response));
            }
            continue;
        }

        const auto session = event.session_id;
        if (game::common::decode_heartbeat_request(event.payload).has_value() &&
            authenticated_.contains(session)) {
            game::common::HeartbeatResponse heartbeat;
            response = game::common::encode(heartbeat, request_id);
            static_cast<void>(runtime.try_send(session, response));
            continue;
        }
        if (const auto request =
                game::common::decode_select_character(event.payload);
            request.has_value() && authenticated_.contains(session) &&
            request->character_id() ==
                development_character_id(authenticated_[session].account_id)) {
            const auto& session_claims = authenticated_[session];
            const auto account_id = session_claims.account_id;
            const auto discovered =
                resolver != nullptr ? resolver->endpoint() : std::nullopt;
            const auto gateway_address = discovered.has_value()
                                             ? discovered->address
                                             : downstream_address_;
            const auto gateway_port =
                discovered.has_value() ? discovered->port : downstream_port_;
            const auto ticket =
                session_claims.correlation_id.has_value()
                    ? tickets_.issue(
                          game::common::TicketPurpose::EnterGame,
                          account_id,
                          1,
                          request->character_id(),
                          *session_claims.correlation_id,
                          std::chrono::seconds(30))
                    : tickets_.issue(
                          game::common::TicketPurpose::EnterGame,
                          account_id,
                          1,
                          request->character_id(),
                          std::chrono::seconds(30));
            game::common::EnterGameIssued issued;
            issued.set_enter_game_ticket(ticket.data(), ticket.size());
            auto* quic_endpoint = issued.add_gateway_endpoints();
            quic_endpoint->set_protocol(
                ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_QUIC);
            quic_endpoint->set_address(gateway_address);
            quic_endpoint->set_port(gateway_port);
            quic_endpoint->set_priority(0);
            auto* tcp_endpoint = issued.add_gateway_endpoints();
            tcp_endpoint->set_protocol(
                ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_TLS_TCP);
            tcp_endpoint->set_address(gateway_address);
            tcp_endpoint->set_port(gateway_port);
            tcp_endpoint->set_priority(1);
            response = game::common::encode(issued, request_id);
        } else {
            game::common::EdgeError error;
            error.set_code(game::common::edge_error_not_authenticated);
            error.set_message("authenticate before selecting character");
            response = game::common::encode(error, request_id);
        }
        static_cast<void>(runtime.try_send(session, response));
    }
}

void ServiceFrame::handle_gateway_events(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    cluster::InstanceBudgetReporter* budget_reporter) {
    for (auto& event : runtime.drain_events(max_events_per_frame_)) {
        // 管线登记/注销先于其余分支:SessionClosed 的额度归还不依赖
        // 业务处理,SessionOpened 只登记不回包。
        if (event.kind == game::gateway::GatewayEventKind::SessionOpened) {
            pipeline_->on_session_opened(event.session_id);
        } else if (
            event.kind == game::gateway::GatewayEventKind::SessionClosed) {
            static_cast<void>(pipeline_->on_session_closed(event.session_id));
        }
        if (!absorb_lifecycle(event)) {
            continue;
        }

        if (const auto attach =
                game::common::decode_edge_attach(event.payload);
            attach.has_value()) {
            handle_edge_attach(logger, runtime, event, *attach);
            continue;
        }

        if (!event.established) {
            const auto request = game::common::decode_enter_game(event.payload);
            const auto redeemed = tickets_.redeem(
                request.has_value()
                    ? game::common::protobuf_bytes(request->enter_game_ticket())
                    : std::span<const std::byte>{},
                game::common::TicketPurpose::EnterGame);
            const auto request_id =
                game::common::edge_request_id(event.payload).value_or(0);
            const bool accepted_ticket =
                redeemed.status == game::common::RedeemStatus::Accepted &&
                redeemed.claims.realm_id == 1 &&
                redeemed.claims.character_id != 0;
            std::vector<std::byte> response;
            if (!accepted_ticket) {
                game::common::EdgeError error;
                error.set_code(
                    game::common::edge_error_invalid_enter_game_ticket);
                error.set_message("invalid or replayed enter-game ticket");
                response = game::common::encode(error, request_id);
            } else {
                authenticated_[event.session_id] = redeemed.claims;
                game::common::EnterGameAccepted accepted;
                accepted.set_account_id(redeemed.claims.account_id);
                accepted.set_character_id(redeemed.claims.character_id);
                response = game::common::encode(accepted, request_id);
                observability::EventContext context{
                    .correlation_id = std::nullopt,
                    .request_id = request_id,
                };
                if (redeemed.claims.correlation_id.has_value()) {
                    context.correlation_id = game::common::correlation_id_hex(
                        *redeemed.claims.correlation_id);
                }
                static_cast<void>(logger.info(
                    "player_session_established",
                    "gateway accepted player session",
                    {observability::field(
                         "account_id",
                         redeemed.claims.account_id,
                         observability::DataClass::Pseudonymous),
                     observability::field(
                         "character_id",
                         redeemed.claims.character_id,
                         observability::DataClass::Pseudonymous)},
                    std::move(context)));
            }
            if (accepted_ticket) {
                static_cast<void>(
                    runtime.try_accept(event.session_id, response));
            } else {
                static_cast<void>(
                    runtime.try_decline(event.session_id, response));
            }
        } else {
            const auto session = event.session_id;
            if (authenticated_.contains(session)) {
                static_cast<void>(runtime.try_send(session, event.payload));
            } else {
                static_cast<void>(runtime.try_close(session));
            }
        }
    }
    // 帧尾发布额度快照(#43):策略节流在 InstanceBudgetReporter 内,
    // 写失败不更新已发布状态,后续帧自动重试。
    if (budget_reporter != nullptr && pipeline_.has_value()) {
        static_cast<void>(budget_reporter->publish(
            cluster::InstanceBudgetSnapshot{
                pipeline_->conn_free(), pipeline_->fetch_free(), true}));
    }
}

void ServiceFrame::handle_edge_attach(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    const game::gateway::GatewayEvent& event,
    const game::common::EdgeAttach& attach) {
    if (!attach_.has_value() && !attach_unavailable_) {
        try {
            attach_.emplace(
                game::common::seed_from_environment(
                    "REALMMESH_IDENTITY_KEY_SEED"),
                game::common::seed_from_environment(
                    "REALMMESH_QUEUE_KEY_SEED"),
                *pipeline_,
                identity_token_kid,
                queue_number_kid,
                "realmmesh/login-verify");
        } catch (const std::exception& error) {
            attach_unavailable_ = true;
            static_cast<void>(logger.warn(
                "edge_attach_rejected",
                "attach verification unavailable: signing seed not set",
                {observability::field(
                    "error_message", std::string(error.what()))}));
        }
    }
    const auto request_id =
        game::common::edge_request_id(event.payload).value_or(0);
    if (!attach_.has_value()) {
        decline_attach(
            runtime,
            event,
            request_id,
            game::common::edge_error_invalid_credentials,
            "attach verification unavailable");
        return;
    }
    switch (attach_->chain.handle(
        event.session_id,
        attach.identity_token(),
        attach.queue_number_token(),
        std::chrono::system_clock::now())) {
    case game::gateway::EdgeAttachVerdict::Accepted: {
        game::common::EdgeAttachAccepted accepted;
        accepted.set_account_id(attach_->chain.last_account_id());
        const auto response = game::common::encode(accepted, request_id);
        if (event.established) {
            static_cast<void>(runtime.try_send(event.session_id, response));
        } else {
            static_cast<void>(runtime.try_accept(event.session_id, response));
        }
        static_cast<void>(logger.info(
            "edge_session_attached",
            "edge session attached",
            {observability::field(
                "account_id",
                attach_->chain.last_account_id(),
                observability::DataClass::Pseudonymous)}));
        break;
    }
    case game::gateway::EdgeAttachVerdict::InvalidCredentials:
    case game::gateway::EdgeAttachVerdict::NotPending:
        decline_attach(
            runtime,
            event,
            request_id,
            game::common::edge_error_invalid_credentials,
            "invalid credentials");
        break;
    case game::gateway::EdgeAttachVerdict::InvalidNumber:
        decline_attach(
            runtime,
            event,
            request_id,
            game::common::edge_error_invalid_queue_number,
            "invalid queue number");
        break;
    case game::gateway::EdgeAttachVerdict::OutOfBudget:
        decline_attach(
            runtime,
            event,
            request_id,
            game::common::edge_error_attach_out_of_budget,
            "attach out of budget");
        break;
    }
}

}  // namespace realm::service_host
