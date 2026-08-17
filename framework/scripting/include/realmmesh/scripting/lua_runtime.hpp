#pragma once

#include <sol/sol.hpp>

#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realm::scripting {

struct ReloadFailure {
    std::string module;
    std::string error;
};

struct ReloadReport {
    std::vector<std::string> reloaded;
    std::vector<ReloadFailure> failures;
};

class LuaRuntime final {
public:
    LuaRuntime();

    LuaRuntime(const LuaRuntime&) = delete;
    LuaRuntime& operator=(const LuaRuntime&) = delete;

    [[nodiscard]] bool load_module(
        std::string name,
        const std::filesystem::path& path,
        std::string* error = nullptr);
    [[nodiscard]] bool load_module_source(
        std::string name,
        std::string_view source,
        std::string* error = nullptr);
    [[nodiscard]] ReloadReport reload_changed();
    [[nodiscard]] bool has_module(std::string_view name) const;
    [[nodiscard]] sol::table module(std::string_view name) const;

    template <typename Function>
    void set_function(std::string_view name, Function&& function) {
        assert_owner_thread();
        lua_.set_function(std::string(name), std::forward<Function>(function));
    }

    template <typename Return = void, typename... Arguments>
    Return call(
        std::string_view module_name,
        std::string_view function_name,
        Arguments&&... arguments) {
        assert_owner_thread();
        const auto exports = module(module_name);
        const sol::object value = exports[std::string(function_name)];
        if (!value.is<sol::protected_function>()) {
            throw std::runtime_error(
                "Lua module function not found: " + std::string(module_name) +
                "." + std::string(function_name));
        }
        sol::protected_function function = value.as<sol::protected_function>();
        sol::protected_function_result result(
            function(std::forward<Arguments>(arguments)...));
        if (!result.valid()) {
            const sol::error error = result;
            throw std::runtime_error(error.what());
        }
        if constexpr (!std::is_void_v<Return>) {
            return result.get<Return>();
        }
    }

private:
    struct Module {
        sol::environment environment;
        sol::table exports;
        std::optional<std::filesystem::path> path;
        std::optional<std::filesystem::file_time_type> last_write;
    };

    [[nodiscard]] std::optional<Module> compile_module(
        std::string_view chunk_name,
        std::string_view source,
        std::string* error);
    void assert_owner_thread() const;

    std::thread::id owner_thread_;
    sol::state lua_;
    std::unordered_map<std::string, Module> modules_;
};

}  // namespace realm::scripting
