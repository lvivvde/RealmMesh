#include "realmmesh/scripting/lua_runtime.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

namespace realm::scripting {
namespace {

struct ChangedModule {
    std::string name;
    std::filesystem::path path;
    std::filesystem::file_time_type write_time;
};

void assign_error(std::string* destination, std::string message) {
    if (destination != nullptr) *destination = std::move(message);
}

std::optional<std::string> read_file(
    const std::filesystem::path& path, std::string* error) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        assign_error(error, "failed to open Lua file: " + path.string());
        return std::nullopt;
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

}  // namespace

LuaRuntime::LuaRuntime()
    : owner_thread_(std::this_thread::get_id()) {
    lua_.open_libraries(
        sol::lib::base,
        sol::lib::coroutine,
        sol::lib::table,
        sol::lib::string,
        sol::lib::math,
        sol::lib::utf8);
    lua_["dofile"] = sol::nil;
    lua_["loadfile"] = sol::nil;
}

bool LuaRuntime::load_module(
    std::string name, const std::filesystem::path& path, std::string* error) {
    assert_owner_thread();
    auto source = read_file(path, error);
    if (!source.has_value()) return false;
    auto candidate = compile_module(path.string(), *source, error);
    if (!candidate.has_value()) return false;

    std::error_code time_error;
    const auto last_write = std::filesystem::last_write_time(path, time_error);
    candidate->path = path;
    if (!time_error) candidate->last_write = last_write;
    modules_.insert_or_assign(std::move(name), std::move(*candidate));
    return true;
}

bool LuaRuntime::load_module_source(
    std::string name, std::string_view source, std::string* error) {
    assert_owner_thread();
    auto candidate = compile_module(name, source, error);
    if (!candidate.has_value()) return false;
    modules_.insert_or_assign(std::move(name), std::move(*candidate));
    return true;
}

ReloadReport LuaRuntime::reload_changed() {
    assert_owner_thread();
    ReloadReport report;
    std::vector<ChangedModule> changed;
    for (const auto& [name, module_value] : modules_) {
        if (!module_value.path.has_value()) continue;
        std::error_code error;
        const auto write_time =
            std::filesystem::last_write_time(*module_value.path, error);
        if (!error && (!module_value.last_write.has_value() ||
                       write_time != *module_value.last_write)) {
            changed.push_back({name, *module_value.path, write_time});
        }
    }

    for (const auto& [name, path, write_time] : changed) {
        std::string error;
        if (load_module(name, path, &error)) {
            report.reloaded.push_back(name);
        } else {
            modules_.at(name).last_write = write_time;
            report.failures.push_back({name, std::move(error)});
        }
    }
    return report;
}

bool LuaRuntime::has_module(std::string_view name) const {
    assert_owner_thread();
    return modules_.contains(std::string(name));
}

sol::table LuaRuntime::module(std::string_view name) const {
    assert_owner_thread();
    const auto iterator = modules_.find(std::string(name));
    if (iterator == modules_.end()) {
        throw std::runtime_error("Lua module not loaded: " + std::string(name));
    }
    return iterator->second.exports;
}

std::optional<LuaRuntime::Module> LuaRuntime::compile_module(
    std::string_view chunk_name, std::string_view source, std::string* error) {
    const sol::load_result loaded = lua_.load(source, std::string(chunk_name));
    if (!loaded.valid()) {
        const sol::error load_error = loaded;
        assign_error(error, load_error.what());
        return std::nullopt;
    }

    sol::environment environment(lua_, sol::create, lua_.globals());
    environment["_G"] = environment;
    sol::protected_function script = loaded.get<sol::protected_function>();
    sol::set_environment(environment, script);
    const sol::protected_function_result result = script();
    if (!result.valid()) {
        const sol::error runtime_error = result;
        assign_error(error, runtime_error.what());
        return std::nullopt;
    }
    const sol::object exports = result.get<sol::object>();
    if (!exports.is<sol::table>()) {
        assign_error(error, "Lua module must return a table");
        return std::nullopt;
    }
    return Module{
        std::move(environment),
        exports.as<sol::table>(),
        std::nullopt,
        std::nullopt};
}

void LuaRuntime::assert_owner_thread() const {
    if (std::this_thread::get_id() != owner_thread_) {
        throw std::logic_error("LuaRuntime accessed from a non-owner thread");
    }
}

}  // namespace realm::scripting
