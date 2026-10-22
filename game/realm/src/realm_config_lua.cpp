#include "realmmesh/game/realm/realm_config_lua.hpp"

#include "realmmesh/game/common/player_data_config_lua.hpp"

#include <sol/sol.hpp>

#include <cstdlib>
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

RealmConfig parse_realm_config(const sol::table& root) {
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
    config.max_events_per_frame = optional_positive(
        root, "max_events_per_frame", config.max_events_per_frame);
    const sol::object scene = table.raw_get<sol::object>("scene");
    if (scene != sol::lua_nil) {
        if (!scene.is<sol::table>())
            throw std::invalid_argument("realm.scene must be table");
        const auto options = scene.as<sol::table>();
        config.scene.enabled = options.get_or("enabled", false);
        const auto dynamic = options.raw_get<sol::object>("dynamic");
        if (dynamic != sol::lua_nil && !dynamic.is<bool>())
            throw std::invalid_argument("realm.scene.dynamic must be boolean");
        config.scene.dynamic = options.get_or("dynamic", false);
        if (config.scene.enabled) {
            const auto text = [&](const char* field, std::string fallback) {
                const auto value = options.get_or(field, std::move(fallback));
                if (value.empty())
                    throw std::invalid_argument("empty Realm Scene option");
                return value;
            };
            const auto path = [&](const char* field) {
                const auto direct = options.get_or(field, std::string{});
                if (!direct.empty()) return direct;
                const auto env = options.get_or(
                    std::string(field) + "_environment", std::string{});
                const char* value =
                    env.empty() ? nullptr : std::getenv(env.c_str());
                if (value == nullptr || *value == '\0')
                    throw std::invalid_argument("missing Realm Scene TLS path");
                return std::string(value);
            };
            config.scene.host = text("host", config.scene.host);
            const auto port =
                optional_positive(options, "port", config.scene.port);
            if (port > 65535) throw std::invalid_argument("invalid Scene port");
            config.scene.port = static_cast<std::uint16_t>(port);
            config.scene.instance_id =
                text("instance_id", config.scene.instance_id);
            config.scene.server_identity =
                text("server_identity", config.scene.server_identity);
            config.scene.ca_file = path("ca_file");
            config.scene.certificate_file = path("certificate_file");
            config.scene.private_key_file = path("private_key_file");
            const auto targets = options.raw_get<sol::object>("targets");
            if (targets != sol::lua_nil) {
                if (!targets.is<sol::table>())
                    throw std::invalid_argument("scene.targets must be array");
                const auto list = targets.as<sol::table>();
                std::size_t count = 0;
                for (const auto& item : list) {
                    if (++count > 1 || !item.first.is<lua_Integer>() ||
                        item.first.as<lua_Integer>() != 1 ||
                        !item.second.is<sol::table>())
                        throw std::invalid_argument(
                            "S3 permits only one additional Scene target");
                    const auto target = item.second.as<sol::table>();
                    RealmConfig::SceneTarget value;
                    value.shard_id = optional_positive(target, "shard_id", 2);
                    const auto target_port =
                        optional_positive(target, "port", value.port);
                    if (value.shard_id != 2 || target_port > 65535)
                        throw std::invalid_argument(
                            "invalid Scene target shard/port");
                    value.port = static_cast<std::uint16_t>(target_port);
                    value.host = target.get_or("host", value.host);
                    value.instance_id =
                        target.get_or("instance_id", value.instance_id);
                    value.server_identity =
                        target.get_or("server_identity", value.server_identity);
                    const auto coordinate = [&](const char* field) {
                        const auto x = target.raw_get<sol::object>(field);
                        if (x == sol::lua_nil) return std::int32_t{0};
                        if (!x.is<lua_Integer>() || x.as<lua_Integer>() < 0 ||
                            x.as<lua_Integer>() > 10000)
                            throw std::invalid_argument(
                                "invalid Scene entry coordinate");
                        return static_cast<std::int32_t>(x.as<lua_Integer>());
                    };
                    value.entry_x = coordinate("entry_x");
                    value.entry_y = coordinate("entry_y");
                    if (value.host.empty() || value.instance_id.empty() ||
                        value.server_identity.empty() ||
                        value.instance_id == config.scene.instance_id)
                        throw std::invalid_argument(
                            "Scene target requires independent identity");
                    config.scene.targets.push_back(std::move(value));
                }
            }
        }
    }
    config.player_data = common::parse_player_data_config(root);
    return config;
}

}  // namespace realm::game::realm
