#include "realmmesh/scheduler/frame_scheduler.hpp"
#include "realmmesh/observability/logger.hpp"

#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string_view>
#include <system_error>
#include <utility>

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

void run_server(
    const ServerConfig& config,
    realm::observability::Logger& logger) {
    realm::scheduler::SteadyFrameClock clock;
    realm::scheduler::FrameScheduler scheduler(config.tick_rate, clock);

    static_cast<void>(logger.info(
        "service_started",
        "RealmMesh scheduler demo started",
        {realm::observability::field("tick_rate", config.tick_rate)}));

    const auto completed_frames = scheduler.run([&](realm::scheduler::FrameContext frame) {
        // Frame pipeline: receive messages -> update state -> run timers -> sync.
        static_cast<void>(logger.log(
            realm::observability::Severity::Debug,
            "frame_completed",
            {},
            {realm::observability::field("frame_index", frame.index)}));
        return config.frame_limit == 0 || frame.index < config.frame_limit;
    });

    static_cast<void>(logger.info(
        "service_stopped",
        "RealmMesh scheduler demo stopped",
        {realm::observability::field("completed_frames", completed_frames)}));
    static_cast<void>(logger.flush(std::chrono::seconds(2)));
}

}  // namespace

int main(int argc, char* argv[]) {
    ServerConfig config;
    if (!parse_arguments(argc, argv, config)) {
        return 1;
    }

    realm::observability::LoggerConfig logging;
    logging.min_severity = realm::observability::Severity::Debug;
    logging.file_path = ".runtime/logs/realmmesh/demo.jsonl";
    logging.console = true;
    realm::observability::Logger logger(
        std::move(logging),
        realm::observability::ServiceIdentity{
            .environment = "development",
            .cluster = "local",
            .region = "local",
            .service_name = "realmmesh",
            .service_instance = "scheduler-demo",
            .node_id = "development-node",
            .zone = "development",
        });
    run_server(config, logger);
    return 0;
}
