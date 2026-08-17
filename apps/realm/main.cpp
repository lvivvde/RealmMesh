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
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

volatile std::sig_atomic_t stop_requested = 0;
void handle_stop_signal(int) { stop_requested = 1; }

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

}  // namespace

int main(int argc, char* argv[]) {
    try {
        auto config = realm::game::gateway::GatewayConfigLoader::load(
            config_path(argc, argv));
        const auto tick_rate = config.tick_rate;
        const auto max_events = config.max_events_per_frame;
        const auto gateway_address = config.downstream_address;
        const auto gateway_port = config.downstream_port;
        if (gateway_address.empty() || gateway_port == 0) {
            throw std::invalid_argument("realm downstream gateway endpoint is required");
        }
        realm::game::common::SessionTicketCodec tickets(load_key());
        realm::game::gateway::GatewayRuntime runtime(std::move(config));
        std::unordered_map<realm::game::gateway::ClientSessionId, std::uint64_t>
            authenticated;

        std::signal(SIGINT, handle_stop_signal);
        std::signal(SIGTERM, handle_stop_signal);
        runtime.start();
        std::cout << "RealmMesh realm/character listening on 0.0.0.0:"
                  << runtime.local_port() << '\n';

        realm::scheduler::SteadyFrameClock clock;
        realm::scheduler::FrameScheduler scheduler(tick_rate, clock);
        static_cast<void>(scheduler.run([&](realm::scheduler::FrameContext) {
            for (auto& event : runtime.drain_events(max_events)) {
                if (!event.client_session_id.has_value()) continue;
                const auto client = *event.client_session_id;
                if (event.kind == realm::network::TransportEventKind::SessionClosed) {
                    authenticated.erase(client);
                    continue;
                }
                if (event.kind != realm::network::TransportEventKind::MessageReceived)
                    continue;

                std::vector<std::byte> response;
                if (const auto request =
                        realm::game::common::decode_realm_authenticate(event.payload);
                    request.has_value()) {
                    const auto claims = tickets.validate(
                        request->login_ticket,
                        realm::game::common::TicketPurpose::Login);
                    if (!claims.has_value() || claims->realm_id != 1) {
                        response = realm::game::common::encode(
                            realm::game::common::EdgeError{2001, "invalid login ticket"});
                    } else {
                        authenticated[client] = claims->account_id;
                        response = realm::game::common::encode(
                            realm::game::common::CharacterList{{
                                {character_id(claims->account_id), "Development Hero"},
                            }});
                    }
                } else if (const auto request =
                               realm::game::common::decode_select_character(event.payload);
                           request.has_value() && authenticated.contains(client) &&
                           request->character_id == character_id(authenticated[client])) {
                    const auto account_id = authenticated[client];
                    response = realm::game::common::encode(
                        realm::game::common::EnterGameIssued{
                            tickets.issue(
                                realm::game::common::TicketPurpose::EnterGame,
                                account_id,
                                1,
                                request->character_id,
                                std::chrono::seconds(30)),
                            {gateway_address, gateway_port},
                        });
                } else {
                    response = realm::game::common::encode(
                        realm::game::common::EdgeError{2002, "authenticate before selecting character"});
                }
                static_cast<void>(runtime.try_send(client, response));
            }
            return stop_requested == 0 && runtime.running();
        }));
        runtime.stop();
        std::cout << "RealmMesh realm/character stopped\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Realm/character failed: " << error.what() << '\n';
        return 1;
    }
}
