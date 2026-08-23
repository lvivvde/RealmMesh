#include "realmmesh/service_host/service_frame.hpp"

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

[[nodiscard]] std::string connection_key(
    std::string_view transport_name, network::SessionId transport_session_id) {
    return std::string(transport_name) + '#' +
           std::to_string(transport_session_id);
}

[[nodiscard]] game::common::SessionTicketKey load_ticket_key() {
    const char* value = std::getenv("REALMMESH_SESSION_TICKET_KEY");
    if (value == nullptr) {
        throw std::runtime_error("REALMMESH_SESSION_TICKET_KEY is not set");
    }
    return game::common::parse_ticket_key_hex(value);
}

[[nodiscard]] bool known_service(std::string_view service_name) {
    return service_name == "login" || service_name == "realm" ||
           service_name == "gateway";
}

/// 未知服务无业务帧,ticket codec 仅为成员占位(不会被调用);
/// codec 拒绝全零 key,故填非零哑值。
[[nodiscard]] game::common::SessionTicketKey make_ticket_key(
    std::string_view service_name) {
    if (!known_service(service_name)) {
        game::common::SessionTicketKey placeholder{};
        placeholder.fill(std::byte{1});
        return placeholder;
    }
    return load_ticket_key();
}

}  // namespace

ServiceFrame::ServiceFrame(
    std::string_view service_name,
    std::string downstream_address,
    std::uint16_t downstream_port,
    std::size_t max_events_per_frame)
    : service_name_(service_name),
      downstream_address_(std::move(downstream_address)),
      downstream_port_(downstream_port),
      max_events_per_frame_(max_events_per_frame),
      tickets_(make_ticket_key(service_name)) {
    if (service_name_ != "gateway" && known_service(service_name_) &&
        (downstream_address_.empty() || downstream_port_ == 0)) {
        throw std::invalid_argument(
            service_name_ + " downstream endpoint is required");
    }
}

void ServiceFrame::started(
    observability::Logger& logger,
    const game::gateway::GatewayRuntime& runtime) const {
    if (service_name_ == "gateway") {
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
    if (!known_service(service_name_)) return;
    static_cast<void>(logger.info(
        "service_started",
        service_name_ + " service started",
        {observability::field("listen_address", "0.0.0.0"),
         observability::field("listen_port", runtime.local_port())}));
}

void ServiceFrame::tick(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    cluster::ServiceResolver* resolver) {
    if (service_name_ == "login") {
        handle_login_events(logger, runtime, resolver);
    } else if (service_name_ == "realm") {
        handle_realm_events(logger, runtime, resolver);
    } else if (service_name_ == "gateway") {
        handle_gateway_events(logger, runtime);
    }
}

void ServiceFrame::stopped(
    observability::Logger& logger,
    const game::gateway::GatewayRuntime& runtime) const {
    if (!known_service(service_name_)) return;
    if (service_name_ == "gateway") {
        const auto stats = runtime.stats();
        static_cast<void>(logger.info(
            "service_stopped",
            "gateway service stopped",
            {observability::field(
                 "overload_disconnects", stats.overload_disconnects),
             observability::field(
                 "outbound_rejected", stats.rejected_outbound_commands),
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
        if (event.kind != game::gateway::GatewayEventKind::MessageReceived) {
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
            error.set_code(1001);
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
        if (event.client_session_id.has_value()) {
            static_cast<void>(
                runtime.try_send(*event.client_session_id, response));
        } else {
            if (authenticated) {
                static_cast<void>(runtime.try_accept_connection(
                    event.transport_name,
                    event.transport_session_id,
                    response));
            } else {
                static_cast<void>(runtime.try_send_channel(
                    event.transport_name,
                    event.transport_session_id,
                    response));
                static_cast<void>(runtime.try_close_channel(
                    event.transport_name, event.transport_session_id));
            }
        }
    }
}

void ServiceFrame::handle_realm_events(
    observability::Logger& logger,
    game::gateway::GatewayRuntime& runtime,
    cluster::ServiceResolver* resolver) {
    for (auto& event : runtime.drain_events(max_events_per_frame_)) {
        if (event.kind == game::gateway::GatewayEventKind::ConnectionClosed) {
            pending_authenticated_.erase(connection_key(
                event.transport_name, event.transport_session_id));
            if (event.client_session_id.has_value()) {
                authenticated_.erase(*event.client_session_id);
            }
            continue;
        }
        if (event.kind ==
            game::gateway::GatewayEventKind::ClientSessionOpened) {
            const auto pending = pending_authenticated_.find(connection_key(
                event.transport_name, event.transport_session_id));
            if (pending != pending_authenticated_.end() &&
                event.client_session_id.has_value()) {
                authenticated_[*event.client_session_id] = pending->second;
                pending_authenticated_.erase(pending);
            }
            continue;
        }
        if (event.kind != game::gateway::GatewayEventKind::MessageReceived) {
            continue;
        }

        const auto request_id =
            game::common::edge_request_id(event.payload).value_or(0);
        std::vector<std::byte> response;
        if (!event.client_session_id.has_value()) {
            const auto request =
                game::common::decode_realm_authenticate(event.payload);
            const auto claims = tickets_.validate(
                request.has_value()
                    ? game::common::protobuf_bytes(request->login_ticket())
                    : std::span<const std::byte>{},
                game::common::TicketPurpose::Login);
            if (!claims.has_value() || claims->realm_id != 1) {
                game::common::EdgeError error;
                error.set_code(2001);
                error.set_message("invalid login ticket");
                response = game::common::encode(error, request_id);
            } else {
                pending_authenticated_[connection_key(
                    event.transport_name, event.transport_session_id)] =
                    *claims;
                game::common::CharacterList characters;
                auto* character = characters.add_characters();
                character->set_id(development_character_id(claims->account_id));
                character->set_name("Development Hero");
                response = game::common::encode(characters, request_id);
                observability::EventContext context{
                    .correlation_id = std::nullopt,
                    .request_id = request_id,
                };
                if (claims->correlation_id.has_value()) {
                    context.correlation_id = game::common::correlation_id_hex(
                        *claims->correlation_id);
                }
                static_cast<void>(logger.info(
                    "player_session_established",
                    "realm authenticated player session",
                    {observability::field(
                        "account_id",
                        claims->account_id,
                        observability::DataClass::Pseudonymous)},
                    std::move(context)));
            }
            if (claims.has_value() && claims->realm_id == 1) {
                static_cast<void>(runtime.try_accept_connection(
                    event.transport_name,
                    event.transport_session_id,
                    response));
            } else {
                static_cast<void>(runtime.try_send_channel(
                    event.transport_name,
                    event.transport_session_id,
                    response));
                static_cast<void>(runtime.try_close_channel(
                    event.transport_name, event.transport_session_id));
            }
            continue;
        }

        const auto client = *event.client_session_id;
        if (game::common::decode_heartbeat_request(event.payload).has_value() &&
            authenticated_.contains(client)) {
            game::common::HeartbeatResponse heartbeat;
            response = game::common::encode(heartbeat, request_id);
            static_cast<void>(runtime.try_send(client, response));
            continue;
        }
        if (const auto request =
                game::common::decode_select_character(event.payload);
            request.has_value() && authenticated_.contains(client) &&
            request->character_id() ==
                development_character_id(authenticated_[client].account_id)) {
            const auto& session_claims = authenticated_[client];
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
            error.set_code(2002);
            error.set_message("authenticate before selecting character");
            response = game::common::encode(error, request_id);
        }
        static_cast<void>(runtime.try_send(client, response));
    }
}

void ServiceFrame::handle_gateway_events(
    observability::Logger& logger, game::gateway::GatewayRuntime& runtime) {
    for (auto& event : runtime.drain_events(max_events_per_frame_)) {
        if (event.kind == game::gateway::GatewayEventKind::ConnectionClosed) {
            pending_authenticated_.erase(connection_key(
                event.transport_name, event.transport_session_id));
            if (event.client_session_id.has_value()) {
                authenticated_.erase(*event.client_session_id);
            }
            continue;
        }
        if (event.kind ==
            game::gateway::GatewayEventKind::ClientSessionOpened) {
            const auto pending = pending_authenticated_.find(connection_key(
                event.transport_name, event.transport_session_id));
            if (pending != pending_authenticated_.end() &&
                event.client_session_id.has_value()) {
                authenticated_[*event.client_session_id] = pending->second;
                pending_authenticated_.erase(pending);
            }
            continue;
        }
        if (event.kind != game::gateway::GatewayEventKind::MessageReceived) {
            continue;
        }

        if (!event.client_session_id.has_value()) {
            const auto request = game::common::decode_enter_game(event.payload);
            const auto claims =
                request.has_value()
                    ? tickets_.validate(
                          game::common::protobuf_bytes(
                              request->enter_game_ticket()),
                          game::common::TicketPurpose::EnterGame)
                    : std::nullopt;
            const auto request_id =
                game::common::edge_request_id(event.payload).value_or(0);
            const bool accepted_ticket =
                claims.has_value() && claims->realm_id == 1 &&
                claims->character_id != 0 && replay_guard_.consume(*claims);
            std::vector<std::byte> response;
            if (!accepted_ticket) {
                game::common::EdgeError error;
                error.set_code(3001);
                error.set_message("invalid or replayed enter-game ticket");
                response = game::common::encode(error, request_id);
            } else {
                pending_authenticated_[connection_key(
                    event.transport_name, event.transport_session_id)] =
                    *claims;
                game::common::EnterGameAccepted accepted;
                accepted.set_account_id(claims->account_id);
                accepted.set_character_id(claims->character_id);
                response = game::common::encode(accepted, request_id);
                observability::EventContext context{
                    .correlation_id = std::nullopt,
                    .request_id = request_id,
                };
                if (claims->correlation_id.has_value()) {
                    context.correlation_id = game::common::correlation_id_hex(
                        *claims->correlation_id);
                }
                static_cast<void>(logger.info(
                    "player_session_established",
                    "gateway accepted player session",
                    {observability::field(
                         "account_id",
                         claims->account_id,
                         observability::DataClass::Pseudonymous),
                     observability::field(
                         "character_id",
                         claims->character_id,
                         observability::DataClass::Pseudonymous)},
                    std::move(context)));
            }
            if (accepted_ticket) {
                static_cast<void>(runtime.try_accept_connection(
                    event.transport_name,
                    event.transport_session_id,
                    response));
            } else {
                static_cast<void>(runtime.try_send_channel(
                    event.transport_name,
                    event.transport_session_id,
                    response));
                static_cast<void>(runtime.try_close_channel(
                    event.transport_name, event.transport_session_id));
            }
        } else {
            const auto client = *event.client_session_id;
            if (authenticated_.contains(client)) {
                static_cast<void>(runtime.try_send(client, event.payload));
            } else {
                static_cast<void>(runtime.try_close_channel(
                    event.transport_name, event.transport_session_id));
            }
        }
    }
}

}  // namespace realm::service_host
