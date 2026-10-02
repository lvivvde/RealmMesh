#include "realmmesh/game/realm/realm_config.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace realm::game::realm {
namespace {

[[nodiscard]] std::size_t optional_positive(
    const sol::table& table, std::string_view field, std::size_t fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<lua_Integer>() || value.as<lua_Integer>() <= 0) {
        throw std::invalid_argument(
            "realm field " + std::string(field) + " must be a positive integer");
    }
    return static_cast<std::size_t>(value.as<lua_Integer>());
}

}  // namespace

RealmConfig RealmConfigLoader::parse(const sol::table& root) {
    const sol::object section = root.raw_get<sol::object>("realm");
    if (!section.is<sol::table>()) {
        throw std::invalid_argument(
            "realm configuration requires a realm table");
    }
    const sol::table table = section.as<sol::table>();
    const sol::object rule = table.raw_get<sol::object>("training_rule_file");
    if (!rule.is<std::string>() || rule.as<std::string>().empty()) {
        throw std::invalid_argument(
            "realm.training_rule_file must be a non-empty string");
    }

    RealmConfig config;
    config.training_rule_file = rule.as<std::string>();
    config.data_workers =
        optional_positive(table, "data_workers", config.data_workers);
    config.data_capacity =
        optional_positive(table, "data_capacity", config.data_capacity);
    config.max_pending_per_session = optional_positive(
        table, "max_pending_per_session", config.max_pending_per_session);
    config.retry_after = std::chrono::seconds{static_cast<std::int64_t>(
        optional_positive(
            table,
            "retry_after_seconds",
            static_cast<std::size_t>(config.retry_after.count())))};
    return config;
}

}  // namespace realm::game::realm
