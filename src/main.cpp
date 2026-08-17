#include "realmmesh/scheduler/frame_scheduler.hpp"

#include <charconv>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <system_error>

namespace {

struct ServerConfig {
    std::uint32_t tick_rate{20};
    std::uint64_t frame_limit{5};
};

template <typename Integer>
bool parse_integer(std::string_view text, Integer& value) {
    const auto* const begin = text.data();
    const auto* const end = begin + text.size();
    const auto [position, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && position == end;
}

void print_usage(std::string_view program) {
    std::cout << "Usage: " << program
              << " [--tick-rate <frames-per-second>] [--frames <count>]\n"
              << "Use --frames 0 to run until the process is stopped.\n";
}

bool parse_arguments(int argc, char* argv[], ServerConfig& config) {
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};

        if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return false;
        }

        if (index + 1 >= argc) {
            std::cerr << "Missing value for " << argument << '\n';
            return false;
        }

        const std::string_view value{argv[++index]};
        if (argument == "--tick-rate") {
            if (!parse_integer(value, config.tick_rate) || config.tick_rate == 0) {
                std::cerr << "Tick rate must be a positive integer.\n";
                return false;
            }
        } else if (argument == "--frames") {
            if (!parse_integer(value, config.frame_limit)) {
                std::cerr << "Frame count must be a non-negative integer.\n";
                return false;
            }
        } else {
            std::cerr << "Unknown option: " << argument << '\n';
            return false;
        }
    }

    return true;
}

void run_server(const ServerConfig& config) {
    realm::scheduler::SteadyFrameClock clock;
    realm::scheduler::FrameScheduler scheduler(config.tick_rate, clock);

    std::cout << "RealmMesh started at " << config.tick_rate << " FPS\n";

    const auto completed_frames = scheduler.run([&config](realm::scheduler::FrameContext frame) {
        // Frame pipeline: receive messages -> update state -> run timers -> sync.
        std::cout << "frame=" << frame.index << '\n';
        return config.frame_limit == 0 || frame.index < config.frame_limit;
    });

    std::cout << "RealmMesh stopped after " << completed_frames << " frames\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    ServerConfig config;
    if (!parse_arguments(argc, argv, config)) {
        return 1;
    }

    run_server(config);
    return 0;
}
