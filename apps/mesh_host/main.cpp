#include "realmmesh/scheduler/frame_scheduler.hpp"
#include "realmmesh/scripting/lua_runtime.hpp"
#include "realmmesh/service_host/mesh_host.hpp"

#include <sol/sol.hpp>

#include <csignal>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

volatile std::sig_atomic_t stop_requested = 0;
void handle_stop_signal(int) { stop_requested = 1; }

/// 命令行选项:--config 默认当前目录下的 configs/。
struct Options {
    std::filesystem::path config_root{"configs/"};
    std::string service;  ///< 非空 = 模式 2 单服务
    realm::service_host::CliOverrides overrides;
};

/// 解析命令行;非法参数(未知选项或缺失取值)返回 nullopt → exit 2。
[[nodiscard]] std::optional<Options> parse_options(int argc, char* argv[]) {
    Options options;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument = argv[index];
        const bool has_value = index + 1 < argc;
        if (argument == "--config" && has_value) {
            options.config_root = argv[++index];
        } else if (argument == "--service" && has_value) {
            options.service = argv[++index];
        } else if (argument == "--instance-id" && has_value) {
            options.overrides.instance_id = argv[++index];
        } else if (argument == "--node-id" && has_value) {
            options.overrides.node_id = argv[++index];
        } else if (argument == "--zone" && has_value) {
            options.overrides.zone = argv[++index];
        } else {
            std::cerr << "realm_mesh: invalid argument: " << argument << '\n';
            return std::nullopt;
        }
    }
    return options;
}

/// 加载 <config_root>/main.config 的 services 表为拓扑描述;
/// 文件缺失、services 表缺失或条目格式错抛异常。
[[nodiscard]] std::vector<realm::service_host::ServiceSpec> load_topology(
    const std::filesystem::path& config_root) {
    realm::scripting::LuaRuntime runtime;
    std::string error;
    if (!runtime.load_module(
            "main_config", config_root / "main.config", &error)) {
        throw std::runtime_error("failed to load main.config: " + error);
    }
    const sol::table root = runtime.module("main_config");
    const sol::object services_value = root.raw_get<sol::object>("services");
    if (!services_value.is<sol::table>()) {
        throw std::invalid_argument("main.config services table is missing");
    }
    const sol::table services = services_value.as<sol::table>();

    std::vector<realm::service_host::ServiceSpec> specs;
    specs.reserve(services.size());
    for (std::size_t index = 1; index <= services.size(); ++index) {
        const sol::object entry_value = services.raw_get<sol::object>(index);
        if (!entry_value.is<sol::table>()) {
            throw std::invalid_argument(
                "main.config services entries must be tables");
        }
        const sol::table entry = entry_value.as<sol::table>();

        realm::service_host::ServiceSpec spec;
        const sol::object name = entry.raw_get<sol::object>("name");
        if (!name.is<std::string>()) {
            throw std::invalid_argument("main.config service name is required");
        }
        spec.name = name.as<std::string>();

        const sol::object dependencies =
            entry.raw_get<sol::object>("depends_on");
        if (dependencies != sol::nil) {
            if (!dependencies.is<sol::table>()) {
                throw std::invalid_argument(
                    "main.config depends_on must be a table");
            }
            const sol::table dependency_table = dependencies.as<sol::table>();
            spec.depends_on.reserve(dependency_table.size());
            for (std::size_t position = 1; position <= dependency_table.size();
                 ++position) {
                const sol::object dependency =
                    dependency_table.raw_get<sol::object>(position);
                if (!dependency.is<std::string>()) {
                    throw std::invalid_argument(
                        "main.config depends_on entries must be strings");
                }
                spec.depends_on.push_back(dependency.as<std::string>());
            }
        }

        const sol::object entry_flag = entry.raw_get<sol::object>("entry");
        if (entry_flag != sol::nil) {
            if (!entry_flag.is<bool>()) {
                throw std::invalid_argument(
                    "main.config entry must be a boolean");
            }
            spec.entry = entry_flag.as<bool>();
        }
        specs.push_back(std::move(spec));
    }
    if (specs.empty()) {
        throw std::invalid_argument("main.config services table is empty");
    }
    return specs;
}

}  // namespace

int main(int argc, char* argv[]) {
    const auto options = parse_options(argc, argv);
    if (!options.has_value()) {
        std::cerr << "usage: realm_mesh [--config <dir>] [--service <name>] "
                     "[--instance-id <id>] [--node-id <id>] [--zone <zone>]\n";
        return 2;
    }

    std::vector<realm::service_host::ServiceSpec> specs;
    try {
        specs = load_topology(options->config_root);
        if (!options->service.empty()) {
            specs = realm::service_host::MeshHost::narrow_single_service(
                std::move(specs), options->service);
        }
    } catch (const std::exception& error) {
        std::cerr << "realm_mesh: " << error.what() << '\n';
        return 1;
    }

    realm::service_host::MeshHost mesh(
        options->config_root, std::move(specs), options->overrides);
    if (!mesh.start_all()) {
        std::cerr << "realm_mesh: failed to start services\n";
        return 1;
    }

    std::signal(SIGINT, handle_stop_signal);
    std::signal(SIGTERM, handle_stop_signal);

    realm::scheduler::SteadyFrameClock clock;
    realm::scheduler::FrameScheduler scheduler(20, clock);
    static_cast<void>(scheduler.run([&](realm::scheduler::FrameContext) {
        mesh.tick();
        return stop_requested == 0;
    }));
    mesh.shutdown();
    return 0;
}
