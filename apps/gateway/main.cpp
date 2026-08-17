#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_config_loader.hpp"
#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/scheduler/frame_scheduler.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace {

volatile std::sig_atomic_t stop_requested = 0;

void handle_stop_signal(int) {
    stop_requested = 1;
}

void print_usage(std::string_view program) {
    std::cout << "Usage: " << program
              << " [--config <lua-file>] [--listen <IPv4-address>] [--port <port>]\n";
}

bool parse_port(std::string_view text, std::uint16_t& port) {
    std::uint32_t value = 0;
    const auto [position, error] = std::from_chars(
        text.data(),
        text.data() + text.size(),
        value);
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
    int argc,
    char* argv[],
    realm::game::gateway::GatewayConfig& config) {
    auto transport_iterator = std::ranges::find_if(
        config.transports,
        [](const auto& transport) {
            return transport.enabled &&
                   transport.protocol == realm::network::TransportProtocol::Tcp;
        });
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
        if (transport_iterator == config.transports.end()) {
            std::cerr << "TCP override requires an enabled TCP transport\n";
            return ParseResult::ExitFailure;
        }
        if (argument == "--listen") {
            transport_iterator->listen_address = value;
        } else if (argument == "--port") {
            if (!parse_port(value, transport_iterator->listen_port)) {
                std::cerr << "Invalid TCP port: " << value << '\n';
                return ParseResult::ExitFailure;
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

}  // namespace

int main(int argc, char* argv[]) {
    try {
        auto config = realm::game::gateway::GatewayConfigLoader::load(
            config_path_from_arguments(argc, argv));
        const auto parse_result = parse_arguments(argc, argv, config);
        if (parse_result != ParseResult::Run) {
            return parse_result == ParseResult::ExitSuccess ? 0 : 1;
        }

        std::signal(SIGINT, handle_stop_signal);
        std::signal(SIGTERM, handle_stop_signal);

        const auto tick_rate = config.tick_rate;
        const auto max_events_per_frame = config.max_events_per_frame;
        realm::game::common::SessionTicketCodec tickets(
            load_session_ticket_key());
        realm::game::common::TicketReplayGuard replay_guard;
        std::unordered_map<
            realm::game::gateway::ClientSessionId,
            realm::game::common::SessionTicketClaims> authenticated;
        realm::game::gateway::GatewayRuntime runtime(std::move(config));
        for (const auto& endpoint : runtime.local_endpoints()) {
            std::cout << "RealmMesh gateway listening on "
                      << endpoint.address << ':' << endpoint.port
                      << " (" << realm::network::to_string(endpoint.protocol)
                      << ", " << endpoint.name << ")\n";
        }

        runtime.start();
        realm::scheduler::SteadyFrameClock frame_clock;
        realm::scheduler::FrameScheduler frame_scheduler(tick_rate, frame_clock);
        bool runtime_failed = false;
        static_cast<void>(frame_scheduler.run([&](realm::scheduler::FrameContext) {
            for (auto& event : runtime.drain_events(max_events_per_frame)) {
                if (event.client_session_id.has_value() &&
                    event.kind == realm::network::TransportEventKind::SessionClosed) {
                    authenticated.erase(*event.client_session_id);
                    continue;
                }
                if (event.kind != realm::network::TransportEventKind::MessageReceived) {
                    continue;
                }

                if (event.client_session_id.has_value()) {
                    const auto client = *event.client_session_id;
                    if (!authenticated.contains(client)) {
                        const auto request =
                            realm::game::common::decode_enter_game(event.payload);
                        const auto claims = request.has_value()
                            ? tickets.validate(
                                  request->enter_game_ticket,
                                  realm::game::common::TicketPurpose::EnterGame)
                            : std::nullopt;
                        std::vector<std::byte> response;
                        if (!claims.has_value() || claims->realm_id != 1 ||
                            claims->character_id == 0 ||
                            !replay_guard.consume(*claims)) {
                            response = realm::game::common::encode(
                                realm::game::common::EdgeError{
                                    3001, "invalid or replayed enter-game ticket"});
                        } else {
                            authenticated.emplace(client, *claims);
                            response = realm::game::common::encode(
                                realm::game::common::EnterGameAccepted{
                                    claims->account_id,
                                    claims->character_id,
                                });
                        }
                        static_cast<void>(runtime.try_send(client, response));
                    } else {
                        static_cast<void>(runtime.try_send(
                            client,
                            event.payload,
                            {.preferred = event.protocol}));
                    }
                } else {
                    const auto response = realm::game::common::encode(
                        realm::game::common::EdgeError{
                            3002, "enter game over TCP before binding another protocol"});
                    static_cast<void>(runtime.try_send_channel(
                        event.transport_name,
                        event.transport_session_id,
                        response));
                }
            }

            runtime_failed = !runtime.running() && stop_requested == 0;
            return stop_requested == 0 && !runtime_failed;
        }));
        runtime.stop();

        const auto stats = runtime.stats();
        std::cout << "RealmMesh gateway I/O stats: udp_dropped="
                  << stats.dropped_udp_messages
                  << ", overload_disconnects=" << stats.overload_disconnects
                  << ", outbound_rejected=" << stats.rejected_outbound_commands
                  << ", tcp_fallback=" << stats.tcp_fallback_deliveries
                  << ", delivery_failed=" << stats.failed_deliveries << '\n';

        std::cout << "RealmMesh gateway stopped\n";
        return runtime_failed ? 1 : 0;
    } catch (const std::exception& error) {
        std::cerr << "Gateway failed: " << error.what() << '\n';
        return 1;
    }
}
