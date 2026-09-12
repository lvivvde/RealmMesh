#include "realmmesh/game/common/account_store.hpp"

#include "realmmesh/scripting/lua_runtime.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>
#include <utility>

namespace realm::game::common {
namespace {

// 统一使用 sol::lua_nil 而非 sol::nil:sol2 在 macOS 上会因 nil 宏已定义
// (version.hpp)导致 sol::nil 在同一工程的不同翻译单元里时有时无。
// 两者是同一个 lua_nil_t 值。与 gateway_config_loader.cpp 同一处理。

// 开发桩既有的派生规则:FNV-1a,零值回绕为 1(账号 id 不得为零)。
[[nodiscard]] std::uint64_t derive_account_id(std::string_view account) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char character : account) {
        hash ^= static_cast<unsigned char>(character);
        hash *= 1099511628211ULL;
    }
    return hash == 0 ? 1 : hash;
}

[[nodiscard]] std::string required_string(
    const sol::table& table, std::string_view field) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (!value.is<std::string>()) {
        throw std::invalid_argument(
            "account field " + std::string(field) + " must be a string");
    }
    return value.as<std::string>();
}

[[nodiscard]] bool optional_boolean(
    const sol::table& table, std::string_view field, bool fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<bool>()) {
        throw std::invalid_argument(
            "account field " + std::string(field) + " must be boolean");
    }
    return value.as<bool>();
}

/// 缺省返回 0,由派生规则补齐;显式给出时必须为正整数。
[[nodiscard]] std::uint64_t optional_account_id(const sol::table& table) {
    const sol::object value = table.raw_get<sol::object>("account_id");
    if (value == sol::lua_nil) return 0;
    if (!value.is<lua_Integer>() || value.as<lua_Integer>() < 1) {
        throw std::invalid_argument(
            "account field account_id must be a positive integer");
    }
    return static_cast<std::uint64_t>(value.as<lua_Integer>());
}

}  // namespace

ConfigAccountStore ConfigAccountStore::load(
    const std::filesystem::path& path) {
    scripting::LuaRuntime runtime;
    std::string error;
    if (!runtime.load_module("account_store_config", path, &error)) {
        throw std::runtime_error("failed to load account store: " + error);
    }
    const sol::table root = runtime.module("account_store_config");

    // 字段级报错统一带上配置路径,排障时一眼定位来源文件。
    ConfigAccountStore store;
    try {
        const sol::object accounts_value = root.raw_get<sol::object>("accounts");
        if (!accounts_value.is<sol::table>()) {
            throw std::invalid_argument("account store requires an accounts table");
        }

        std::unordered_set<std::uint64_t> seen_ids;
        for (const auto& [key, value] : accounts_value.as<sol::table>()) {
            static_cast<void>(key);
            if (!value.is<sol::table>()) {
                throw std::invalid_argument("account entries must be tables");
            }
            const sol::table entry = value.as<sol::table>();
            const std::string account = required_string(entry, "account");

            AccountRecord record;
            record.account_id = optional_account_id(entry);
            if (record.account_id == 0) {
                record.account_id = derive_account_id(account);
            }
            record.banned = optional_boolean(entry, "banned", false);
            record.whitelisted = optional_boolean(entry, "whitelisted", false);

            if (!seen_ids.insert(record.account_id).second) {
                throw std::invalid_argument(
                    "duplicate account_id for account " + account);
            }
            Entry account_entry;
            account_entry.credential = required_string(entry, "credential");
            account_entry.record = record;
            if (!store.accounts_.emplace(account, std::move(account_entry))
                     .second) {
                throw std::invalid_argument("duplicate account " + account);
            }
        }
    } catch (const std::invalid_argument& e) {
        throw std::invalid_argument(path.string() + ": " + e.what());
    }
    return store;
}

std::optional<AccountRecord> ConfigAccountStore::authenticate(
    std::string_view account, std::string_view credential) const {
    const auto entry = accounts_.find(std::string(account));
    if (entry == accounts_.end() || entry->second.credential != credential) {
        return std::nullopt;
    }
    return entry->second.record;
}

}  // namespace realm::game::common
