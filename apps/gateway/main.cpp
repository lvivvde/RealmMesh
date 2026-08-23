#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/cluster/service_publisher.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_config_loader.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/scheduler/frame_scheduler.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

volatile std::sig_atomic_t stop_requested = 0;
volatile std::sig_atomic_t reload_requested = 0;

void handle_stop_signal(int) { stop_requested = 1; }
void handle_reload_signal(int) { reload_requested = 1; }

void print_usage(std::string_view program) {
    std::cout
        << "Usage: " << program
        << " [--config <lua-file>] [--listen <IPv4-address>] [--port <port>]\n";
}

bool parse_port(std::string_view text, std::uint16_t& port) {
    std::uint32_t value = 0;
    const auto [position, error] =
        std::from_chars(text.data(), text.data() + text.size(), value);
    if (error != std::errc{} || position != text.data() + text.size() ||
        value > std::numeric_limits<std::uint16_t>::max()) {
        return false;
    }

    port = static_cast<std::uint16_t>(value);
    return true;
}

enum class ParseResult {
    Run,
    ExitSuccess,
    ExitFailure,
};

ParseResult parse_arguments(
    int argc, char* argv[], realm::game::gateway::GatewayConfig& config) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return ParseResult::ExitSuccess;
        }
        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return ParseResult::ExitFailure;
        }

        const std::string_view value(argv[++index]);
        if (argument == "--config") {
            continue;
        }
        const auto enabled_count =
            std::ranges::count_if(config.transports, [](const auto& transport) {
                return transport.enabled;
            });
        if (enabled_count == 0) {
            std::cerr << "Address override requires an enabled transport\n";
            return ParseResult::ExitFailure;
        }
        if (argument == "--listen") {
            for (auto& transport : config.transports) {
                if (transport.enabled) transport.listen_address = value;
            }
        } else if (argument == "--port") {
            std::uint16_t port = 0;
            if (!parse_port(value, port)) {
                std::cerr << "Invalid edge port: " << value << '\n';
                return ParseResult::ExitFailure;
            }
            for (auto& transport : config.transports) {
                if (transport.enabled) transport.listen_port = port;
            }
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return ParseResult::ExitFailure;
        }
    }
    return ParseResult::Run;
}

std::filesystem::path config_path_from_arguments(int argc, char* argv[]) {
    std::filesystem::path path(REALMMESH_DEFAULT_GATEWAY_CONFIG);
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--config") {
            path = argv[index + 1];
            ++index;
        }
    }
    return path;
}

realm::game::common::SessionTicketKey load_session_ticket_key() {
    const char* value = std::getenv("REALMMESH_SESSION_TICKET_KEY");
    if (value == nullptr) {
        throw std::runtime_error("REALMMESH_SESSION_TICKET_KEY is not set");
    }
    return realm::game::common::parse_ticket_key_hex(value);
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
        const auto path = config_path_from_arguments(argc, argv);
        auto config = realm::game::gateway::GatewayConfigLoader::load(path);
        logger = std::make_unique<realm::observability::Logger>(
            config.logging, config.logging_identity);
        const auto parse_result = parse_arguments(argc, argv, config);
        if (parse_result != ParseResult::Run) {
            return parse_result == ParseResult::ExitSuccess ? 0 : 1;
        }
        if (config.logging_metrics.port != 0) {
            metrics =
                std::make_unique<realm::observability::LoggerMetricsServer>(
                    *logger, config.logging_metrics);
        }

        std::signal(SIGINT, handle_stop_signal);
        std::signal(SIGTERM, handle_stop_signal);
        std::signal(SIGHUP, handle_reload_signal);

        const auto tick_rate = config.tick_rate;
        const auto max_events_per_frame = config.max_events_per_frame;
        const auto discovery_config = config.service_discovery;
        realm::game::common::SessionTicketCodec tickets(
            load_session_ticket_key());
        realm::game::common::TicketReplayGuard replay_guard;
        std::unordered_map<
            realm::game::gateway::ClientSessionId,
            realm::game::common::SessionTicketClaims>
            authenticated;
        std::
            unordered_map<std::string, realm::game::common::SessionTicketClaims>
                pending_authenticated;
        realm::game::gateway::GatewayRuntime runtime(std::move(config));
        std::unique_ptr<realm::cluster::EtcdServiceRegistry> registry;
        std::unique_ptr<realm::cluster::ServicePublisher> publisher;
        if (discovery_config.enabled) {
            registry = std::make_unique<realm::cluster::EtcdServiceRegistry>(
                realm::cluster::make_etcd_registry_options(discovery_config));
            publisher = std::make_unique<realm::cluster::ServicePublisher>(
                *registry,
                realm::cluster::make_service_instance(
                    realm::cluster::ServiceType::Gateway,
                    discovery_config,
                    runtime.local_endpoints(),
                    "0.1.0"),
                discovery_config.lease_ttl);
            const bool registered = publisher->tick();
            if (!registered && discovery_config.required) {
                throw std::runtime_error(
                    "gateway service registration failed: " +
                    registry->last_error());
            }
            if (!registered) {
                static_cast<void>(logger->warn(
                    "dependency_state_changed",
                    "service discovery unavailable; continuing without registration",
                    {realm::observability::field("dependency", "etcd"),
                     realm::observability::field("state", "unavailable"),
                     realm::observability::field(
                         "error_message", registry->last_error())}));
            }
        }
        for (const auto& endpoint : runtime.local_endpoints()) {
            static_cast<void>(logger->info(
                "listener_started",
                "gateway listener started",
                {realm::observability::field(
                     "listen_address", endpoint.address),
                 realm::observability::field("listen_port", endpoint.port),
                 realm::observability::field(
                     "transport", realm::network::to_string(endpoint.protocol)),
                 realm::observability::field(
                     "transport_name", endpoint.name)}));
        }

        runtime.start();
        static_cast<void>(
            logger->info("service_started", "gateway service started"));
        realm::scheduler::SteadyFrameClock frame_clock;
        realm::scheduler::FrameScheduler frame_scheduler(
            tick_rate, frame_clock);
        bool runtime_failed = false;
        static_cast<
            void>(frame_scheduler.run([&](realm::scheduler::FrameContext) {
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
            for (auto& event : runtime.drain_events(max_events_per_frame)) {
                if (event.kind ==
                    realm::game::gateway::GatewayEventKind::ConnectionClosed) {
                    pending_authenticated.erase(connection_key(
                        event.transport_name, event.transport_session_id));
                    if (event.client_session_id.has_value()) {
                        authenticated.erase(*event.client_session_id);
                    }
                    continue;
                }
                if (event.kind == realm::game::gateway::GatewayEventKind::
                                      ClientSessionOpened) {
                    const auto pending =
                        pending_authenticated.find(connection_key(
                            event.transport_name, event.transport_session_id));
                    if (pending != pending_authenticated.end() &&
                        event.client_session_id.has_value()) {
                        authenticated[*event.client_session_id] =
                            pending->second;
                        pending_authenticated.erase(pending);
                    }
                    continue;
                }
                if (event.kind !=
                    realm::game::gateway::GatewayEventKind::MessageReceived) {
                    continue;
                }

                if (!event.client_session_id.has_value()) {
                    const auto request =
                        realm::game::common::decode_enter_game(event.payload);
                    const auto claims =
                        request.has_value()
                            ? tickets.validate(
                                  realm::game::common::protobuf_bytes(
                                      request->enter_game_ticket()),
                                  realm::game::common::TicketPurpose::EnterGame)
                            : std::nullopt;
                    const auto request_id =
                        realm::game::common::edge_request_id(event.payload)
                            .value_or(0);
                    const bool accepted_ticket = claims.has_value() &&
                                                 claims->realm_id == 1 &&
                                                 claims->character_id != 0 &&
                                                 replay_guard.consume(*claims);
                    std::vector<std::byte> response;
                    if (!accepted_ticket) {
                        realm::game::common::EdgeError error;
                        error.set_code(3001);
                        error.set_message(
                            "invalid or replayed enter-game ticket");
                        response =
                            realm::game::common::encode(error, request_id);
                    } else {
                        pending_authenticated[connection_key(
                            event.transport_name, event.transport_session_id)] =
                            *claims;
                        realm::game::common::EnterGameAccepted accepted;
                        accepted.set_account_id(claims->account_id);
                        accepted.set_character_id(claims->character_id);
                        response =
                            realm::game::common::encode(accepted, request_id);
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
                            "gateway accepted player session",
                            {realm::observability::field(
                                 "account_id",
                                 claims->account_id,
                                 realm::observability::DataClass::Pseudonymous),
                             realm::observability::field(
                                 "character_id",
                                 claims->character_id,
                                 realm::observability::DataClass::
                                     Pseudonymous)},
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
                    if (authenticated.contains(client)) {
                        static_cast<void>(
                            runtime.try_send(client, event.payload));
                    } else {
                        static_cast<void>(runtime.try_close_channel(
                            event.transport_name, event.transport_session_id));
                    }
                }
            }

            runtime_failed = !runtime.running() && stop_requested == 0;
            return stop_requested == 0 && !runtime_failed;
        }));
        if (const auto error = runtime.terminal_error(); error.has_value()) {
            static_cast<void>(logger->error(
                "runtime_io_failed",
                "gateway I/O loop terminated unexpectedly",
                {realm::observability::field("error_message", *error)}));
        }
        runtime.stop();

        const auto stats = runtime.stats();
        static_cast<void>(logger->info(
            "service_stopped",
            "gateway service stopped",
            {realm::observability::field(
                 "overload_disconnects", stats.overload_disconnects),
             realm::observability::field(
                 "outbound_rejected", stats.rejected_outbound_commands),
             realm::observability::field(
                 "delivered", stats.successful_deliveries),
             realm::observability::field(
                 "delivery_failed", stats.failed_deliveries)}));
        static_cast<void>(logger->flush(std::chrono::seconds(2)));
        return runtime_failed ? 1 : 0;
    } catch (const std::exception& error) {
        if (logger != nullptr) {
            static_cast<void>(logger->error(
                "service_start_failed",
                "gateway service failed",
                {realm::observability::field("error_message", error.what())}));
            static_cast<void>(logger->flush(std::chrono::seconds(2)));
        } else {
            std::cerr << "Gateway failed: " << error.what() << '\n';
        }
        return 1;
    }
}
