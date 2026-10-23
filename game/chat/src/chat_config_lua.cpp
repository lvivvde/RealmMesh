#include "realmmesh/game/chat/chat_config.hpp"
#include "realmmesh/service_host/layered_config_lua.hpp"
#include <sol/sol.hpp>
#include <limits>
#include <set>
#include <stdexcept>
namespace realm::game::chat {
ChatConfig load_chat_config(const service_host::LayeredConfig& config) {
    const auto value = service_host::lua_root(config).raw_get<sol::object>("chat");
    if (!value.is<sol::table>()) throw std::invalid_argument("chat must be a table");
    const auto table = value.as<sol::table>();
    ChatConfig c;
    const auto number = [&table](const char* key, std::uint64_t fallback) {
        const auto item = table.raw_get<sol::object>(key);
        if (item == sol::lua_nil) return fallback;
        if (!item.is<lua_Integer>() || item.as<lua_Integer>() < 0)
            throw std::invalid_argument("chat numeric option must be an unsigned integer");
        return static_cast<std::uint64_t>(item.as<lua_Integer>());
    };
    const auto string = [&table](const char* key, std::string fallback) {
        const auto item = table.raw_get<sol::object>(key);
        if (item == sol::lua_nil) return fallback;
        if (!item.is<std::string>()) throw std::invalid_argument("chat identity must be a string");
        return item.as<std::string>();
    };
    const auto realm = number("realm_id", c.realm_id);
    const auto timeout = number("operation_timeout_ms", c.operation_timeout_ms);
    if (realm > std::numeric_limits<std::uint32_t>::max() || timeout > common::chat_operation_timeout_ms)
        throw std::invalid_argument("chat numeric option out of range");
    c.realm_id = static_cast<std::uint32_t>(realm);
    c.realm_instance_id = string("realm_instance_id", c.realm_instance_id);
    c.realm_identity = string("realm_identity", c.realm_identity);
    c.operation_timeout_ms = static_cast<std::uint32_t>(timeout);
    c.pending_requests = number("pending_requests", c.pending_requests);
    c.queued_per_link = number("queued_per_link", c.queued_per_link);
    c.pending_pushes = number("pending_pushes", c.pending_pushes);
    c.pending_push_bytes = number("pending_push_bytes", c.pending_push_bytes);
    const auto channels = table.raw_get<sol::object>("channels");
    if (!channels.is<sol::table>()) throw std::invalid_argument("chat.channels must be an array");
    c.channels.clear();
    std::set<lua_Integer> indices;
    for (const auto& item : channels.as<sol::table>()) {
        if (!item.first.is<lua_Integer>() || item.first.as<lua_Integer>() < 1 || item.first.as<lua_Integer>() > 3 ||
            !item.second.is<lua_Integer>() || item.second.as<lua_Integer>() < 0 || item.second.as<lua_Integer>() > 2)
            throw std::invalid_argument("chat.channels must be an integer array");
        indices.insert(item.first.as<lua_Integer>());
        c.channels.push_back(static_cast<std::uint64_t>(item.second.as<lua_Integer>()));
    }
    if (!indices.empty() && *indices.rbegin() != static_cast<lua_Integer>(indices.size()))
        throw std::invalid_argument("chat.channels must be a dense array");
    validate_chat_config(c);
    return c;
}
}  // namespace realm::game::chat
