#include "realmmesh/game/common/player_data_config.hpp"

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

    config.database_file = optional_string(table, "database_file");
    const auto bootstrap = optional_string(table, "bootstrap_accounts_file");
    if (!bootstrap.empty()) config.options.bootstrap_accounts_file = bootstrap;
    const auto cost = optional_string(table, "credential_hash_cost");
    if (!cost.empty()) {
        config.options.credential_hash_cost = parse_credential_hash_cost(cost);
    }

    const sol::object timeout = table.raw_get<sol::object>("busy_timeout_ms");
    if (timeout != sol::lua_nil) {
        if (!timeout.is<lua_Integer>() || timeout.as<lua_Integer>() <= 0) {
            throw std::invalid_argument(
                "player_data busy_timeout_ms must be a positive integer");
        }
        config.options.busy_timeout =
            std::chrono::milliseconds{timeout.as<lua_Integer>()};
    }
    return config;
}

void resolve_player_data_paths(
    PlayerDataConfig& config, const std::filesystem::path& config_root) {
    resolve(config.database_file, config_root);
    if (config.options.bootstrap_accounts_file.has_value()) {
        resolve(*config.options.bootstrap_accounts_file, config_root);
    }
}

}  // namespace realm::game::common
