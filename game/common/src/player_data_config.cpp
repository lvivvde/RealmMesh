#include "realmmesh/game/common/player_data_config.hpp"

#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>

namespace realm::game::common {
namespace {

[[nodiscard]] std::string optional_string(
    const sol::table& table, std::string_view field) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return {};
    if (!value.is<std::string>()) {
        throw std::invalid_argument(
            "player_data field " + std::string(field) + " must be a string");
    }
    return value.as<std::string>();
}

[[nodiscard]] std::optional<std::chrono::milliseconds> optional_timeout(
    const sol::table& table, std::string_view field) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return std::nullopt;
    if (!value.is<lua_Integer>() || value.as<lua_Integer>() <= 0 ||
        value.as<lua_Integer>() > std::numeric_limits<std::int32_t>::max()) {
        throw std::invalid_argument(
            "player_data " + std::string(field) +
            " must be a positive 32-bit integer");
    }
    return std::chrono::milliseconds{value.as<lua_Integer>()};
}

void resolve(std::filesystem::path& path, const std::filesystem::path& root) {
    if (!path.empty() && path.is_relative()) path = root / path;
}

}  // namespace

CredentialHashCost parse_credential_hash_cost(std::string_view value) {
    if (value == "interactive") return CredentialHashCost::Interactive;
    if (value == "minimum") return CredentialHashCost::Minimum;
    throw std::invalid_argument(
        "player_data credential_hash_cost must be interactive or minimum");
}

PlayerDataConfig parse_player_data_config(const sol::table& root) {
    PlayerDataConfig config;
    const sol::object section = root.raw_get<sol::object>("player_data");
    if (section == sol::lua_nil) return config;
    if (!section.is<sol::table>()) {
        throw std::invalid_argument("player_data configuration must be a table");
    }
    const auto table = section.as<sol::table>();

    // SQLite 时代的键(ADR-0010)在 ADR-0011 后没有意义；静默忽略会让旧配置
    // 以为自己仍指向本地文件。
    for (const auto* retired : {"database_file", "busy_timeout_ms"}) {
        if (table.raw_get<sol::object>(retired) != sol::lua_nil) {
            throw std::invalid_argument(
                std::string("player_data ") + retired +
                " is retired; configure uri and database (ADR-0011)");
        }
    }

    config.uri = optional_string(table, "uri");
    const auto uri_environment = optional_string(table, "uri_environment");
    if (!uri_environment.empty()) {
        if (const char* value = std::getenv(uri_environment.c_str())) {
            if (*value == '\0') {
                throw std::invalid_argument(
                    "player_data URI environment variable is empty: " +
                    uri_environment);
            }
            config.uri = value;
        } else if (config.uri.empty()) {
            throw std::invalid_argument(
                "player_data URI environment variable is not set: " +
                uri_environment);
        }
    }
    config.database = optional_string(table, "database");
    if (!config.uri.empty() && config.database.empty()) {
        throw std::invalid_argument(
            "player_data database is required when uri is set");
    }
    if (const auto timeout =
            optional_timeout(table, "server_selection_timeout_ms")) {
        config.options.server_selection_timeout = *timeout;
    }
    if (const auto timeout = optional_timeout(table, "socket_timeout_ms")) {
        config.options.socket_timeout = *timeout;
    }
    const auto bootstrap = optional_string(table, "bootstrap_accounts_file");
    if (!bootstrap.empty()) config.options.bootstrap_accounts_file = bootstrap;
    const auto cost = optional_string(table, "credential_hash_cost");
    if (!cost.empty()) {
        config.options.credential_hash_cost = parse_credential_hash_cost(cost);
    }
    return config;
}

void resolve_player_data_paths(
    PlayerDataConfig& config, const std::filesystem::path& config_root) {
    if (config.options.bootstrap_accounts_file.has_value()) {
        resolve(*config.options.bootstrap_accounts_file, config_root);
    }
}

}  // namespace realm::game::common
