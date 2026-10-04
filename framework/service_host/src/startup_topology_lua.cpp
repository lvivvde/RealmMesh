#include "realmmesh/service_host/startup_topology.hpp"

#include "realmmesh/scripting/lua_runtime.hpp"

#include <sol/sol.hpp>

#include <stdexcept>
#include <string>
#include <utility>

namespace realm::service_host {

std::vector<ServiceSpec> load_topology(
    const std::filesystem::path& config_root) {
    scripting::LuaRuntime runtime;
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

    std::vector<ServiceSpec> specs;
    specs.reserve(services.size());
    for (std::size_t index = 1; index <= services.size(); ++index) {
        const sol::object entry_value = services.raw_get<sol::object>(index);
        if (!entry_value.is<sol::table>()) {
            throw std::invalid_argument(
                "main.config services entries must be tables");
        }
        const sol::table entry = entry_value.as<sol::table>();

        ServiceSpec spec;
        const sol::object name = entry.raw_get<sol::object>("name");
        if (!name.is<std::string>()) {
            throw std::invalid_argument("main.config service name is required");
        }
        spec.name = name.as<std::string>();

        const sol::object dependencies =
            entry.raw_get<sol::object>("depends_on");
        if (dependencies != sol::lua_nil) {
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
        if (entry_flag != sol::lua_nil) {
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

}  // namespace realm::service_host
