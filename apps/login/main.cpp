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
#include <stdexcept>
#include <string>
#include <string_view>
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
    return REALMMESH_DEFAULT_LOGIN_CONFIG;
}

realm::game::common::SessionTicketKey load_key() {
    const char* value = std::getenv("REALMMESH_SESSION_TICKET_KEY");
    if (value == nullptr) {
        throw std::runtime_error("REALMMESH_SESSION_TICKET_KEY is not set");
    }
    return realm::game::common::parse_ticket_key_hex(value);
}

std::uint64_t development_account_id(std::string_view account) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : account) {
        const auto value = static_cast<unsigned char>(character);
        hash ^= value;
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
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
        const auto fallback_realm_address = config.downstream_address;
        const auto fallback_realm_port = config.downstream_port;
        const auto discovery_config = config.service_discovery;
        if (fallback_realm_address.empty() || fallback_realm_port == 0) {
            throw std::invalid_argument("login downstream realm endpoint is required");
        }
        realm::game::common::SessionTicketCodec tickets(load_key());
        realm::game::gateway::GatewayRuntime runtime(std::move(config));
        std::unique_ptr<realm::cluster::EtcdServiceRegistry> registry;
        std::unique_ptr<realm::cluster::ServicePublisher> publisher;
        std::unique_ptr<realm::cluster::ServiceResolver> realm_resolver;
        if (discovery_config.enabled) {
            registry = std::make_unique<realm::cluster::EtcdServiceRegistry>(
                realm::cluster::make_etcd_registry_options(discovery_config));
            publisher = std::make_unique<realm::cluster::ServicePublisher>(
                *registry,
                realm::cluster::make_service_instance(
                    realm::cluster::ServiceType::Login,
                    discovery_config,
                    runtime.local_endpoints(),
                    "0.1.0"),
                discovery_config.lease_ttl);
            const bool registered = publisher->tick();
            if (!registered && discovery_config.required) {
                throw std::runtime_error(
                    "login service registration failed: " +
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
            realm_resolver = std::make_unique<realm::cluster::ServiceResolver>(
                *registry,
                realm::cluster::ServiceType::Realm,
                realm::network::TransportProtocol::TlsTcp);
        }

        std::signal(SIGINT, handle_stop_signal);
        std::signal(SIGTERM, handle_stop_signal);
        std::signal(SIGHUP, handle_reload_signal);
        runtime.start();
        static_cast<void>(logger->info(
            "service_started",
            "login service started",
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
                if (event.kind != realm::game::gateway::GatewayEventKind::MessageReceived) {
                    continue;
                }

                const auto request =
                    realm::game::common::decode_login_request(event.payload);
                const auto request_id =
                    realm::game::common::edge_request_id(event.payload).value_or(0);
                std::vector<std::byte> response;
                const bool authenticated =
                    request.has_value() && !request->account().empty() &&
                    request->credential() == "dev";
                if (!authenticated) {
                    realm::game::common::EdgeError error;
                    error.set_code(1001);
                    error.set_message("invalid credentials");
                    response = realm::game::common::encode(error, request_id);
                } else {
                    const auto account_id = development_account_id(request->account());
                    const auto correlation_id =
                        realm::game::common::make_correlation_id();
                    const auto correlation_text =
                        realm::game::common::correlation_id_hex(correlation_id);
                    const auto discovered = realm_resolver != nullptr
                        ? realm_resolver->endpoint()
                        : std::nullopt;
                    const auto realm_address = discovered.has_value()
                        ? discovered->address
                        : fallback_realm_address;
                    const auto realm_port = discovered.has_value()
                        ? discovered->port
                        : fallback_realm_port;
                    const auto ticket = tickets.issue(
                        realm::game::common::TicketPurpose::Login,
                        account_id,
                        1,
                        0,
                        correlation_id,
                        std::chrono::seconds(60));
                    realm::game::common::LoginSucceeded success;
                    success.set_account_id(account_id);
                    success.set_login_ticket(ticket.data(), ticket.size());
                    auto* endpoint = success.add_realm_endpoints();
                    endpoint->set_protocol(
                        ::realmmesh::protocol::edge::v1::
                            TRANSPORT_PROTOCOL_TLS_TCP);
                    endpoint->set_address(realm_address);
                    endpoint->set_port(realm_port);
                    endpoint->set_priority(0);
                    response = realm::game::common::encode(success, request_id);
                    static_cast<void>(logger->info(
                        "player_session_established",
                        "login authenticated player session",
                        {realm::observability::field(
                            "account_id",
                            account_id,
                            realm::observability::DataClass::Pseudonymous)},
                        realm::observability::EventContext{
                            .correlation_id = correlation_text,
                            .request_id = request_id,
                        }));
                }
                if (event.client_session_id.has_value()) {
                    static_cast<void>(runtime.try_send(
                        *event.client_session_id, response));
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
                            event.transport_name,
                            event.transport_session_id));
                    }
                }
            }
            return stop_requested == 0 && runtime.running();
        }));
        const bool runtime_failed = !runtime.running() && stop_requested == 0;
        if (const auto error = runtime.terminal_error(); error.has_value()) {
            static_cast<void>(logger->error(
                "runtime_io_failed",
                "login I/O loop terminated unexpectedly",
                {realm::observability::field("error_message", *error)}));
        }
        runtime.stop();
        static_cast<void>(logger->info(
            "service_stopped", "login service stopped"));
        static_cast<void>(logger->flush(std::chrono::seconds(2)));
        return runtime_failed ? 1 : 0;
    } catch (const std::exception& error) {
        if (logger != nullptr) {
            static_cast<void>(logger->error(
                "service_start_failed",
                "login service failed",
                {realm::observability::field(
                    "error_message", error.what())}));
            static_cast<void>(logger->flush(std::chrono::seconds(2)));
        } else {
            std::cerr << "Login failed: " << error.what() << '\n';
        }
        return 1;
    }
}
