#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/cluster/service_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_config_loader.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/scheduler/frame_scheduler.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t reload_requested = 0;
void handle_stop_signal(int) { stop_requested = 1; }
void handle_reload_signal(int) { reload_requested = 1; }

std::filesystem::path config_path(int argc, char* argv[]) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--config") return argv[index + 1];
    }
    return REALMMESH_DEFAULT_REALM_CONFIG;
}

realm::game::common::SessionTicketKey load_key() {
    const char* value = std::getenv("REALMMESH_SESSION_TICKET_KEY");
    if (value == nullptr) {
        throw std::runtime_error("REALMMESH_SESSION_TICKET_KEY is not set");
    }
    return realm::game::common::parse_ticket_key_hex(value);
}

std::uint64_t character_id(std::uint64_t account_id) {
    return account_id ^ 0x524d434841524143ULL;
}

std::string connection_key(
    std::string_view transport_name,
    realm::network::SessionId transport_session_id) {
    return std::string(transport_name) + '#' +
           std::to_string(transport_session_id);
}

}  // namespace

int main(int argc, char* argv[]) {
    std::unique_ptr<realm::observability::Logger> logger;
    std::unique_ptr<realm::observability::LoggerMetricsServer> metrics;
    try {
        const auto path = config_path(argc, argv);
        auto config = realm::game::gateway::GatewayConfigLoader::load(path);
        logger = std::make_unique<realm::observability::Logger>(
            config.logging, config.logging_identity);
        if (config.logging_metrics.port != 0) {
            metrics = std::make_unique<
                realm::observability::LoggerMetricsServer>(
                *logger, config.logging_metrics);
        }
        const auto tick_rate = config.tick_rate;
        const auto max_events = config.max_events_per_frame;
        const auto fallback_gateway_address = config.downstream_address;
        const auto fallback_gateway_port = config.downstream_port;
        const auto discovery_config = config.service_discovery;
        if (fallback_gateway_address.empty() || fallback_gateway_port == 0) {
            throw std::invalid_argument("realm downstream gateway endpoint is required");
        }
        realm::game::common::SessionTicketCodec tickets(load_key());
        realm::game::gateway::GatewayRuntime runtime(std::move(config));
        std::unique_ptr<realm::cluster::EtcdServiceRegistry> registry;
        std::unique_ptr<realm::cluster::ServicePublisher> publisher;
        std::unique_ptr<realm::cluster::ServiceResolver> gateway_resolver;
        if (discovery_config.enabled) {
            registry = std::make_unique<realm::cluster::EtcdServiceRegistry>(
                realm::cluster::make_etcd_registry_options(discovery_config));
            publisher = std::make_unique<realm::cluster::ServicePublisher>(
                *registry,
                realm::cluster::make_service_instance(
                    realm::cluster::ServiceType::Realm,
                    discovery_config,
                    runtime.local_endpoints(),
                    "0.1.0"),
                discovery_config.lease_ttl);
            const bool registered = publisher->tick();
            if (!registered && discovery_config.required) {
                throw std::runtime_error(
                    "realm service registration failed: " +
                    registry->last_error());
            }
            if (!registered) {
                static_cast<void>(logger->warn(
                    "dependency_state_changed",
                    "service discovery unavailable; using Lua fallback",
                    {realm::observability::field("dependency", "etcd"),
                     realm::observability::field(
                         "state", "unavailable"),
                     realm::observability::field(
                         "error_message", registry->last_error())}));
            }
            gateway_resolver = std::make_unique<realm::cluster::ServiceResolver>(
                *registry,
                realm::cluster::ServiceType::Gateway,
                realm::network::TransportProtocol::TlsTcp);
        }
        std::unordered_map<
            realm::game::gateway::ClientSessionId,
            realm::game::common::SessionTicketClaims> authenticated;
        std::unordered_map<
            std::string,
            realm::game::common::SessionTicketClaims> pending_authenticated;

        std::signal(SIGINT, handle_stop_signal);
        std::signal(SIGTERM, handle_stop_signal);
        std::signal(SIGHUP, handle_reload_signal);
        runtime.start();
        static_cast<void>(logger->info(
            "service_started",
            "realm service started",
            {realm::observability::field("listen_address", "0.0.0.0"),
             realm::observability::field("listen_port", runtime.local_port())}));

        realm::scheduler::SteadyFrameClock clock;
        realm::scheduler::FrameScheduler scheduler(tick_rate, clock);
        static_cast<void>(scheduler.run([&](realm::scheduler::FrameContext) {
            if (reload_requested != 0) {
                reload_requested = 0;
                static_cast<void>(runtime.try_reload_credentials());
                try {
                    const auto reloaded =
                        realm::game::gateway::GatewayConfigLoader::load(path);
                    logger->reconfigure({
                        .min_severity = reloaded.logging.min_severity,
                        .module_levels =
                            std::move(reloaded.logging.module_levels),
                        .sample_rates =
                            std::move(reloaded.logging.sample_rates),
                    });
                } catch (const std::exception& error) {
                    static_cast<void>(logger->error(
                        "configuration_reload_failed",
                        "logging configuration reload failed",
                        {realm::observability::field(
                            "error_message", error.what())}));
                }
            }
            if (publisher != nullptr) static_cast<void>(publisher->tick());
            for (auto& event : runtime.drain_events(max_events)) {
                if (event.kind == realm::game::gateway::GatewayEventKind::ConnectionClosed) {
                    pending_authenticated.erase(connection_key(
                        event.transport_name, event.transport_session_id));
                    if (event.client_session_id.has_value()) {
                        authenticated.erase(*event.client_session_id);
                    }
                    continue;
                }
                if (event.kind ==
                    realm::game::gateway::GatewayEventKind::ClientSessionOpened) {
                    const auto pending = pending_authenticated.find(connection_key(
                        event.transport_name, event.transport_session_id));
                    if (pending != pending_authenticated.end() &&
                        event.client_session_id.has_value()) {
                        authenticated[*event.client_session_id] = pending->second;
                        pending_authenticated.erase(pending);
                    }
                    continue;
                }
                if (event.kind != realm::game::gateway::GatewayEventKind::MessageReceived)
                    continue;

                const auto request_id =
                    realm::game::common::edge_request_id(event.payload).value_or(0);
                std::vector<std::byte> response;
                if (!event.client_session_id.has_value()) {
                    const auto request =
                        realm::game::common::decode_realm_authenticate(event.payload);
                    const auto claims = tickets.validate(
                        request.has_value()
                            ? realm::game::common::protobuf_bytes(
                                  request->login_ticket())
                            : std::span<const std::byte>{},
                        realm::game::common::TicketPurpose::Login);
                    if (!claims.has_value() || claims->realm_id != 1) {
                        realm::game::common::EdgeError error;
                        error.set_code(2001);
                        error.set_message("invalid login ticket");
                        response = realm::game::common::encode(error, request_id);
                    } else {
                        pending_authenticated[connection_key(
                            event.transport_name,
                            event.transport_session_id)] = *claims;
                        realm::game::common::CharacterList characters;
                        auto* character = characters.add_characters();
                        character->set_id(character_id(claims->account_id));
                        character->set_name("Development Hero");
                        response = realm::game::common::encode(
                            characters, request_id);
                        realm::observability::EventContext context{
                            .correlation_id = std::nullopt,
                            .request_id = request_id,
                        };
                        if (claims->correlation_id.has_value()) {
                            context.correlation_id =
                                realm::game::common::correlation_id_hex(
                                    *claims->correlation_id);
                        }
                        static_cast<void>(logger->info(
                            "player_session_established",
                            "realm authenticated player session",
                            {realm::observability::field(
                                "account_id",
                                claims->account_id,
                                realm::observability::DataClass::Pseudonymous)},
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
                            event.transport_name,
                            event.transport_session_id));
                    }
                    continue;
                }

                const auto client = *event.client_session_id;
                if (const auto request =
                               realm::game::common::decode_select_character(event.payload);
                           request.has_value() && authenticated.contains(client) &&
                           request->character_id() ==
                               character_id(authenticated[client].account_id)) {
                    const auto& session_claims = authenticated[client];
                    const auto account_id = session_claims.account_id;
                    const auto discovered = gateway_resolver != nullptr
                        ? gateway_resolver->endpoint()
                        : std::nullopt;
                    const auto gateway_address = discovered.has_value()
                        ? discovered->address
                        : fallback_gateway_address;
                    const auto gateway_port = discovered.has_value()
                        ? discovered->port
                        : fallback_gateway_port;
                    const auto ticket = session_claims.correlation_id.has_value()
                        ? tickets.issue(
                              realm::game::common::TicketPurpose::EnterGame,
                              account_id,
                              1,
                              request->character_id(),
                              *session_claims.correlation_id,
                              std::chrono::seconds(30))
                        : tickets.issue(
                              realm::game::common::TicketPurpose::EnterGame,
                              account_id,
                              1,
                              request->character_id(),
                              std::chrono::seconds(30));
                    realm::game::common::EnterGameIssued issued;
                    issued.set_enter_game_ticket(ticket.data(), ticket.size());
                    auto* quic_endpoint = issued.add_gateway_endpoints();
                    quic_endpoint->set_protocol(
                        ::realmmesh::protocol::edge::v1::TRANSPORT_PROTOCOL_QUIC);
                    quic_endpoint->set_address(gateway_address);
                    quic_endpoint->set_port(gateway_port);
                    quic_endpoint->set_priority(0);
                    auto* tcp_endpoint = issued.add_gateway_endpoints();
                    tcp_endpoint->set_protocol(
                        ::realmmesh::protocol::edge::v1::
                            TRANSPORT_PROTOCOL_TLS_TCP);
                    tcp_endpoint->set_address(gateway_address);
                    tcp_endpoint->set_port(gateway_port);
                    tcp_endpoint->set_priority(1);
                    response = realm::game::common::encode(issued, request_id);
                } else {
                    realm::game::common::EdgeError error;
                    error.set_code(2002);
                    error.set_message(
                        "authenticate before selecting character");
                    response = realm::game::common::encode(error, request_id);
                }
                static_cast<void>(runtime.try_send(client, response));
            }
            return stop_requested == 0 && runtime.running();
        }));
        const bool runtime_failed = !runtime.running() && stop_requested == 0;
        if (const auto error = runtime.terminal_error(); error.has_value()) {
            static_cast<void>(logger->error(
                "runtime_io_failed",
                "realm I/O loop terminated unexpectedly",
                {realm::observability::field("error_message", *error)}));
        }
        runtime.stop();
        static_cast<void>(logger->info(
            "service_stopped", "realm service stopped"));
        static_cast<void>(logger->flush(std::chrono::seconds(2)));
        return runtime_failed ? 1 : 0;
    } catch (const std::exception& error) {
        if (logger != nullptr) {
            static_cast<void>(logger->error(
                "service_start_failed",
                "realm service failed",
                {realm::observability::field(
                    "error_message", error.what())}));
            static_cast<void>(logger->flush(std::chrono::seconds(2)));
        } else {
            std::cerr << "Realm/character failed: " << error.what() << '\n';
        }
        return 1;
    }
}
