#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/cluster/service_publisher.hpp"
#include "realmmesh/cluster/service_resolver.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_config_loader.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/scheduler/frame_scheduler.hpp"

#include <chrono>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace {

volatile std::sig_atomic_t stop_requested = 0;
void handle_stop_signal(int) { stop_requested = 1; }

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
    try {
        auto config = realm::game::gateway::GatewayConfigLoader::load(
            config_path(argc, argv));
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
                std::cerr << "Login service discovery unavailable; using Lua "
                             "fallback endpoint: "
                          << registry->last_error() << '\n';
            }
            realm_resolver = std::make_unique<realm::cluster::ServiceResolver>(
                *registry,
                realm::cluster::ServiceType::Realm,
                realm::network::TransportProtocol::Tcp);
        }

        std::signal(SIGINT, handle_stop_signal);
        std::signal(SIGTERM, handle_stop_signal);
        runtime.start();
        std::cout << "RealmMesh login listening on 0.0.0.0:"
                  << runtime.local_port() << " (development authenticator)\n";

        realm::scheduler::SteadyFrameClock clock;
        realm::scheduler::FrameScheduler scheduler(tick_rate, clock);
        static_cast<void>(scheduler.run([&](realm::scheduler::FrameContext) {
            if (publisher != nullptr) static_cast<void>(publisher->tick());
            for (auto& event : runtime.drain_events(max_events)) {
                if (event.kind != realm::network::TransportEventKind::MessageReceived ||
                    !event.client_session_id.has_value()) continue;

                const auto request =
                    realm::game::common::decode_login_request(event.payload);
                std::vector<std::byte> response;
                if (!request.has_value() || request->account.empty() ||
                    request->credential != "dev") {
                    response = realm::game::common::encode(
                        realm::game::common::EdgeError{1001, "invalid credentials"});
                } else {
                    const auto account_id = development_account_id(request->account);
                    const auto discovered = realm_resolver != nullptr
                        ? realm_resolver->endpoint()
                        : std::nullopt;
                    const auto realm_address = discovered.has_value()
                        ? discovered->address
                        : fallback_realm_address;
                    const auto realm_port = discovered.has_value()
                        ? discovered->port
                        : fallback_realm_port;
                    response = realm::game::common::encode(
                        realm::game::common::LoginSucceeded{
                            account_id,
                            tickets.issue(
                                realm::game::common::TicketPurpose::Login,
                                account_id,
                                1,
                                0,
                                std::chrono::seconds(60)),
                            {realm_address, realm_port},
                        });
                }
                static_cast<void>(runtime.try_send(
                    *event.client_session_id, response));
            }
            return stop_requested == 0 && runtime.running();
        }));
        runtime.stop();
        std::cout << "RealmMesh login stopped\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Login failed: " << error.what() << '\n';
        return 1;
    }
}
