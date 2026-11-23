#include "realmmesh/game/common/player_data_store.hpp"

#include "realmmesh/scripting/lua_runtime.hpp"

#include <bsoncxx/builder/basic/document.hpp>
#include <bsoncxx/builder/basic/kvp.hpp>
#include <bsoncxx/document/element.hpp>
#include <bsoncxx/document/value.hpp>
#include <bsoncxx/document/view.hpp>
#include <bsoncxx/exception/exception.hpp>
#include <bsoncxx/types.hpp>
#include <mongocxx/client.hpp>
#include <mongocxx/client_session.hpp>
#include <mongocxx/collection.hpp>
#include <mongocxx/database.hpp>
#include <mongocxx/exception/exception.hpp>
#include <mongocxx/exception/operation_exception.hpp>
#include <mongocxx/instance.hpp>
#include <mongocxx/options/find.hpp>
#include <mongocxx/options/find_one_and_update.hpp>
#include <mongocxx/options/index.hpp>
#include <mongocxx/options/transaction.hpp>
#include <mongocxx/options/update.hpp>
#include <mongocxx/pool.hpp>
#include <mongocxx/read_concern.hpp>
#include <mongocxx/uri.hpp>
#include <mongocxx/write_concern.hpp>
#include <sodium.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace realm::game::common {
namespace {

using bsoncxx::builder::basic::kvp;
using bsoncxx::builder::basic::make_document;

constexpr std::string_view accounts_collection = "accounts";
constexpr std::string_view characters_collection = "characters";
constexpr std::string_view metadata_collection = "store_metadata";
constexpr std::string_view bootstrap_marker = "legacy_account_bootstrap";
constexpr std::string_view schema_marker = "schema";
/// 2:角色带 exp / last_training_seq,(realm_id, name) 唯一(#93)。
constexpr std::int64_t schema_version = 2;
constexpr std::size_t character_id_attempts = 8;
constexpr int duplicate_key_code = 11000;

/// 领域编号是 uint64,库内以 int64 按位存放：取值范围不收窄(ADR-0011)。
[[nodiscard]] std::int64_t database_id(
    std::uint64_t value, std::string_view field) {
    if (value == 0) {
        throw PlayerDataError(std::string(field) + " must be non-zero");
    }
    return std::bit_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint64_t domain_id(
    const bsoncxx::document::element& element) {
    if (!element || element.type() != bsoncxx::type::k_int64 ||
        element.get_int64().value == 0) {
        throw PlayerDataError("database contains an invalid id");
    }
    return std::bit_cast<std::uint64_t>(element.get_int64().value);
}

/// 经验、训练序号等非负计数;库内同样以 int64 存放。
[[nodiscard]] std::int64_t database_counter(
    std::uint64_t value, std::string_view field) {
    if (value >
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        throw PlayerDataError(std::string(field) + " is out of range");
    }
    return static_cast<std::int64_t>(value);
}

[[nodiscard]] std::uint64_t domain_counter(
    const bsoncxx::document::element& element) {
    if (!element || element.type() != bsoncxx::type::k_int64 ||
        element.get_int64().value < 0) {
        throw PlayerDataError("database contains an invalid counter");
    }
    return static_cast<std::uint64_t>(element.get_int64().value);
}

[[nodiscard]] bool domain_flag(const bsoncxx::document::element& element) {
    if (!element || element.type() != bsoncxx::type::k_bool) {
        throw PlayerDataError("database contains an invalid account flag");
    }
    return element.get_bool().value;
}

[[nodiscard]] std::string domain_string(
    const bsoncxx::document::element& element) {
    if (!element || element.type() != bsoncxx::type::k_string) {
        throw PlayerDataError("database contains an invalid string field");
    }
    return std::string(element.get_string().value);
}

[[nodiscard]] RealmCharacter realm_character_from(
    const bsoncxx::document::view& view) {
    return RealmCharacter{
        .character_id = domain_id(view["_id"]),
        .name = domain_string(view["name"]),
        .exp = domain_counter(view["exp"]),
        .last_training_seq = domain_counter(view["last_training_seq"]),
    };
}

/// 随机非零 63 位角色编号:最高位恒 0,库内 int64 存放后仍为正数。
[[nodiscard]] std::uint64_t random_character_id() {
    std::uint64_t value = 0;
    while (value == 0) {
        randombytes_buf(&value, sizeof(value));
        value &= 0x7FFF'FFFF'FFFF'FFFFULL;
    }
    return value;
}

[[nodiscard]] bsoncxx::types::b_string text(std::string_view value) {
    return bsoncxx::types::b_string{value};
}

[[nodiscard]] bool is_duplicate_key(const mongocxx::operation_exception& error) {
    return error.code().value() == duplicate_key_code;
}

/// 驱动与 BSON 异常统一映射为 PlayerDataError,调用方(Login Verifier 503、
/// Gateway Unavailable)只需识别这一种数据源故障。
template <typename Function>
decltype(auto) translate_driver_errors(Function&& function) {
    try {
        return std::forward<Function>(function)();
    } catch (const mongocxx::exception& error) {
        throw PlayerDataError(std::string("mongodb: ") + error.what());
    } catch (const bsoncxx::exception& error) {
        throw PlayerDataError(std::string("bson: ") + error.what());
    }
}

/// 每进程只能存在一个驱动实例；三个服务在同一进程内(测试、all-in-one)
/// 各自构造存储时共享它。
void ensure_driver_instance() {
    static const mongocxx::instance instance{};
}

[[nodiscard]] std::int32_t timeout_ms(
    std::chrono::milliseconds timeout, std::string_view field) {
    if (timeout <= std::chrono::milliseconds::zero() ||
        timeout.count() > std::numeric_limits<std::int32_t>::max()) {
        throw PlayerDataError(std::string(field) + " must be positive");
    }
    return static_cast<std::int32_t>(timeout.count());
}

/// 超时只从 player_data 配置进入 URI;URI 自带同名参数会让两处配置
/// 互相覆盖，直接拒绝。
[[nodiscard]] mongocxx::uri configured_uri(
    std::string uri, const MongoPlayerDataOptions& options) {
    if (uri.empty()) throw PlayerDataError("player data uri must not be empty");
    const auto server_selection = timeout_ms(
        options.server_selection_timeout, "server_selection_timeout");
    const auto socket = timeout_ms(options.socket_timeout, "socket_timeout");
    const mongocxx::uri parsed{uri};
    if (parsed.server_selection_timeout_ms().has_value() ||
        parsed.socket_timeout_ms().has_value()) {
        throw PlayerDataError(
            "player data uri must not set serverSelectionTimeoutMS or "
            "socketTimeoutMS; configure player_data timeouts instead");
    }
    const auto scheme = uri.find("://");
    const auto hosts = scheme == std::string::npos ? 0 : scheme + 3;
    if (uri.find('/', hosts) == std::string::npos) uri += '/';
    if (uri.find('?') == std::string::npos) {
        uri += '?';
    } else if (uri.back() != '?' && uri.back() != '&') {
        uri += '&';
    }
    uri += "serverSelectionTimeoutMS=" + std::to_string(server_selection) +
           "&socketTimeoutMS=" + std::to_string(socket);
    return mongocxx::uri{uri};
}

[[nodiscard]] std::string credential_hash(
    std::string_view credential, CredentialHashCost cost) {
    if (credential.empty()) {
        throw PlayerDataError("credential must not be empty");
    }
    const bool minimum = cost == CredentialHashCost::Minimum;
    std::array<char, crypto_pwhash_STRBYTES> output{};
    if (crypto_pwhash_str_alg(
            output.data(),
            credential.data(),
            static_cast<unsigned long long>(credential.size()),
            minimum ? crypto_pwhash_OPSLIMIT_MIN
                    : crypto_pwhash_OPSLIMIT_INTERACTIVE,
            minimum ? crypto_pwhash_MEMLIMIT_MIN
                    : crypto_pwhash_MEMLIMIT_INTERACTIVE,
            crypto_pwhash_ALG_DEFAULT) != 0) {
        throw PlayerDataError("failed to hash account credential");
    }
    return output.data();
}

[[nodiscard]] bool credential_matches(
    std::string_view hash, std::string_view credential) {
    return crypto_pwhash_str_verify(
               hash.data(),
               credential.data(),
               static_cast<unsigned long long>(credential.size())) == 0;
}

[[nodiscard]] std::uint64_t derived_account_id(std::string_view account) {
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
    if (!value.is<std::string>() || value.as<std::string>().empty()) {
        throw PlayerDataError(
            "bootstrap field " + std::string(field) +
            " must be a non-empty string");
    }
    return value.as<std::string>();
}

[[nodiscard]] bool optional_boolean(
    const sol::table& table, std::string_view field, bool fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<bool>()) {
        throw PlayerDataError(
            "bootstrap field " + std::string(field) + " must be boolean");
    }
    return value.as<bool>();
}

[[nodiscard]] std::uint64_t optional_account_id(
    const sol::table& table, std::string_view account) {
    const sol::object value = table.raw_get<sol::object>("account_id");
    if (value == sol::lua_nil) return derived_account_id(account);
    if (!value.is<lua_Integer>() || value.as<lua_Integer>() <= 0) {
        throw PlayerDataError("bootstrap account_id must be positive");
    }
    return static_cast<std::uint64_t>(value.as<lua_Integer>());
}

struct BootstrapAccount final {
    AccountProvisioning account;
    std::string hash;
};

[[nodiscard]] std::vector<BootstrapAccount> load_bootstrap_accounts(
    const std::filesystem::path& path, CredentialHashCost cost) {
    scripting::LuaRuntime runtime;
    std::string error;
    if (!runtime.load_module("player_data_bootstrap", path, &error)) {
        throw PlayerDataError("failed to load bootstrap accounts: " + error);
    }
    const auto root = runtime.module("player_data_bootstrap");
    const sol::object accounts_value = root.raw_get<sol::object>("accounts");
    if (!accounts_value.is<sol::table>()) {
        throw PlayerDataError("bootstrap requires an accounts table");
    }

    std::unordered_set<std::string> names;
    std::unordered_set<std::uint64_t> ids;
    std::vector<BootstrapAccount> accounts;
    for (const auto& [key, value] : accounts_value.as<sol::table>()) {
        static_cast<void>(key);
        if (!value.is<sol::table>()) {
            throw PlayerDataError("bootstrap account entries must be tables");
        }
        const sol::table entry = value.as<sol::table>();
        AccountProvisioning account;
        account.account_name = required_string(entry, "account");
        account.credential = required_string(entry, "credential");
        account.account_id = optional_account_id(entry, account.account_name);
        account.banned = optional_boolean(entry, "banned", false);
        account.whitelisted =
            optional_boolean(entry, "whitelisted", false);
        if (!names.insert(account.account_name).second ||
            !ids.insert(account.account_id).second) {
            throw PlayerDataError("bootstrap account names and ids must be unique");
        }
        accounts.push_back(
            BootstrapAccount{account, credential_hash(account.credential, cost)});
    }
    return accounts;
}

}  // namespace

namespace {
constexpr std::string_view scenes_collection = "character_scenes";
constexpr std::string_view shards_collection = "scene_shards";
constexpr std::string_view scene_instances_collection = "scene_instances";

bool valid_scene_text(std::string_view value) {
    return !value.empty() && value.size() <= 128 &&
           std::ranges::all_of(value, [](unsigned char c) {
               return c >= 33 && c <= 126;
           });
}

bool valid_scene_owner(const SceneOwner& owner) {
    return valid_scene_text(owner.realm_incarnation) &&
           owner.session_binding != 0 &&
           valid_scene_text(owner.scene_instance_id) &&
           valid_scene_text(owner.scene_incarnation) &&
           owner.shard_version <= scene_counter_max;
}

bool valid_scene_location(const SceneLocation& location) {
    return location.shard_id != 0 && location.x >= 0 &&
           location.x <= scene_coordinate_max && location.y >= 0 &&
           location.y <= scene_coordinate_max;
}

bool valid_scene_command(const SceneCommand& command) {
    if (command.seq == 0 || command.seq > scene_counter_max) return false;
    switch (command.type) {
    case SceneCommandType::Enter:
        return command.x == 0 && command.y == 0 && command.target_shard_id == 0;
    case SceneCommandType::Move:
        return command.target_shard_id == 0 && command.x >= 0 &&
               command.x <= scene_coordinate_max && command.y >= 0 &&
               command.y <= scene_coordinate_max;
    case SceneCommandType::Transfer:
        return command.x == 0 && command.y == 0 && command.target_shard_id != 0;
    }
    return false;
}

bsoncxx::document::value scene_identity(const SceneCharacterKey& key) {
    return make_document(
        kvp("realm_id", std::int64_t{key.realm_id}),
        kvp("character_id", database_id(key.character_id, "character_id")));
}

bsoncxx::document::value scene_filter(const SceneCharacterKey& key) {
    if (key.realm_id == 0) throw PlayerDataError("realm_id must be non-zero");
    return make_document(
        kvp("_id", scene_identity(key)),
        kvp("account_id", database_id(key.account_id, "account_id")));
}

bsoncxx::document::value scene_owner_document(const SceneOwner& owner) {
    bsoncxx::builder::basic::document fields;
    fields.append(
        kvp("realm_incarnation", owner.realm_incarnation),
        kvp("session_binding",
            database_id(owner.session_binding, "session_binding")),
        kvp("scene_instance_id", owner.scene_instance_id),
        kvp("scene_incarnation", owner.scene_incarnation));
    if (owner.shard_version != 0)
        fields.append(
            kvp("shard_version",
                database_counter(owner.shard_version, "shard_version")));
    return fields.extract();
}

bsoncxx::document::value scene_location_document(
    const SceneLocation& location) {
    return make_document(
        kvp("shard_id", database_id(location.shard_id, "shard_id")),
        kvp("x", std::int64_t{location.x}),
        kvp("y", std::int64_t{location.y}));
}

bsoncxx::document::value scene_result_document(
    const SceneStoredResult& result) {
    return make_document(
        kvp("type", static_cast<std::int64_t>(result.command.type)),
        kvp("seq", database_counter(result.command.seq, "scene_seq")),
        kvp("x", std::int64_t{result.command.x}),
        kvp("y", std::int64_t{result.command.y}),
        kvp("target_shard_id",
            std::bit_cast<std::int64_t>(result.command.target_shard_id)),
        kvp("digest", scene_command_digest(result.command)),
        kvp("binding_epoch",
            database_counter(result.binding_epoch, "binding_epoch")),
        kvp("owner", scene_owner_document(result.owner)),
        kvp("location", scene_location_document(result.location)));
}

bsoncxx::document::value scene_fields(const SceneStateRecord& state) {
    bsoncxx::builder::basic::document fields;
    fields.append(
        kvp("scene_schema", std::int64_t{1}),
        kvp("account_id",
            database_id(state.character.account_id, "account_id")),
        kvp("binding_epoch",
            database_counter(state.binding_epoch, "binding_epoch")));
    if (state.owner)
        fields.append(kvp("owner", scene_owner_document(*state.owner)));
    else
        fields.append(kvp("owner", bsoncxx::types::b_null{}));
    if (state.confirmed_location)
        fields.append(kvp(
            "location", scene_location_document(*state.confirmed_location)));
    else
        fields.append(kvp("location", bsoncxx::types::b_null{}));
    if (state.last_result)
        fields.append(
            kvp("last_result", scene_result_document(*state.last_result)));
    else
        fields.append(kvp("last_result", bsoncxx::types::b_null{}));
    fields.append(
        kvp("last_scene_seq",
            database_counter(
                state.last_result ? state.last_result->command.seq : 0,
                "scene_seq")));
    return fields.extract();
}

bsoncxx::document::view scene_document(
    const bsoncxx::document::element& field) {
    if (!field || field.type() != bsoncxx::type::k_document)
        throw PlayerDataError("invalid scene document");
    return field.get_document().value;
}

std::int32_t scene_coordinate(const bsoncxx::document::element& field) {
    const auto value = domain_counter(field);
    if (value > static_cast<std::uint64_t>(scene_coordinate_max))
        throw PlayerDataError("invalid scene coordinate");
    return static_cast<std::int32_t>(value);
}

SceneOwner scene_owner_from(bsoncxx::document::view view) {
    SceneOwner owner{
        domain_string(view["realm_incarnation"]),
        domain_id(view["session_binding"]),
        domain_string(view["scene_instance_id"]),
        domain_string(view["scene_incarnation"])};
    if (view["shard_version"])
        owner.shard_version = domain_counter(view["shard_version"]);
    if (!valid_scene_owner(owner)) throw PlayerDataError("invalid scene owner");
    return owner;
}

SceneLocation scene_location_from(bsoncxx::document::view view) {
    return {
        domain_id(view["shard_id"]),
        scene_coordinate(view["x"]),
        scene_coordinate(view["y"])};
}

SceneStateRecord scene_state_from(
    const SceneCharacterKey& key, bsoncxx::document::view view) {
    if (domain_counter(view["scene_schema"]) != 1 ||
        domain_id(view["account_id"]) != key.account_id) {
        throw PlayerDataError("invalid scene schema or ownership");
    }
    SceneStateRecord state{
        .character = key,
        .binding_epoch = domain_counter(view["binding_epoch"])};
    if (!view["owner"] || !view["location"] || !view["last_result"])
        throw PlayerDataError("missing scene fields");
    if (view["owner"].type() != bsoncxx::type::k_null)
        state.owner = scene_owner_from(scene_document(view["owner"]));
    if (view["location"].type() != bsoncxx::type::k_null)
        state.confirmed_location =
            scene_location_from(scene_document(view["location"]));
    if (view["last_result"].type() != bsoncxx::type::k_null) {
        const auto receipt = scene_document(view["last_result"]);
        const auto type = domain_counter(receipt["type"]);
        if (type < 1 || type > 3)
            throw PlayerDataError("invalid scene command type");
        SceneStoredResult result{
            .command =
                {static_cast<SceneCommandType>(type),
                 domain_counter(receipt["seq"]),
                 scene_coordinate(receipt["x"]),
                 scene_coordinate(receipt["y"]),
                 std::bit_cast<std::uint64_t>(
                     receipt["target_shard_id"].get_int64().value)},
            .location =
                scene_location_from(scene_document(receipt["location"])),
            .binding_epoch = domain_counter(receipt["binding_epoch"]),
            .owner = scene_owner_from(scene_document(receipt["owner"]))};
        if (!valid_scene_command(result.command) || result.binding_epoch == 0 ||
            result.binding_epoch > state.binding_epoch ||
            domain_string(receipt["digest"]) !=
                scene_command_digest(result.command)) {
            throw PlayerDataError("invalid scene receipt");
        }
        state.last_result = std::move(result);
    }
    const auto seq = state.last_result ? state.last_result->command.seq : 0;
    if (domain_counter(view["last_scene_seq"]) != seq ||
        state.confirmed_location.has_value() != state.last_result.has_value() ||
        (state.last_result &&
         *state.confirmed_location != state.last_result->location) ||
        (state.owner &&
         (!state.last_result ||
          state.binding_epoch != state.last_result->binding_epoch ||
          *state.owner != state.last_result->owner)) ||
        (state.binding_epoch == 0 && state.last_result)) {
        throw PlayerDataError("inconsistent scene state");
    }
    return state;
}

bsoncxx::document::value scene_cas_filter(
    const SceneCharacterKey& key,
    std::uint64_t epoch,
    const std::optional<SceneOwner>& owner) {
    bsoncxx::builder::basic::document filter;
    filter.append(
        bsoncxx::builder::basic::concatenate(scene_filter(key).view()),
        kvp("binding_epoch", database_counter(epoch, "binding_epoch")));
    if (owner)
        filter.append(kvp("owner", scene_owner_document(*owner)));
    else
        filter.append(kvp("owner", bsoncxx::types::b_null{}));
    return filter.extract();
}

SceneWriteOutcome check_scene_transition(
    const SceneStateRecord& state, const SceneWrite& write) {
    const auto seq = state.last_result ? state.last_result->command.seq : 0;
    if (resolve_scene_write(write, state) == SceneWriteResolution::Committed &&
        state.owner == write.owner &&
        state.binding_epoch == state.last_result->binding_epoch) {
        return SceneWriteOutcome::Replayed;
    }
    if (state.binding_epoch != write.expected_epoch ||
        state.owner != write.expected_owner)
        return SceneWriteOutcome::StaleBinding;
    if (seq == scene_counter_max || write.command.seq != seq + 1)
        return SceneWriteOutcome::InvalidSequence;
    if (write.command.type == SceneCommandType::Enter) {
        if (state.confirmed_location &&
            *state.confirmed_location != write.location)
            return SceneWriteOutcome::InvalidCommand;
    } else {
        if (!state.owner || !state.confirmed_location)
            return SceneWriteOutcome::StaleBinding;
        if (write.command.type == SceneCommandType::Move) {
            if (write.owner != *state.owner ||
                write.location.shard_id != state.confirmed_location->shard_id ||
                write.location.x != write.command.x ||
                write.location.y != write.command.y)
                return SceneWriteOutcome::InvalidCommand;
        } else {
            if (write.owner.realm_incarnation !=
                    state.owner->realm_incarnation ||
                write.owner.session_binding != state.owner->session_binding ||
                write.location.shard_id != write.command.target_shard_id ||
                write.location.shard_id == state.confirmed_location->shard_id)
                return SceneWriteOutcome::InvalidCommand;
        }
    }
    if (write.command.type != SceneCommandType::Move &&
        state.binding_epoch == scene_counter_max) {
        throw PlayerDataError("scene binding epoch exhausted");
    }
    return SceneWriteOutcome::Committed;
}
}  // namespace

namespace {
bsoncxx::document::value instance_fields(const SceneInstanceState& record) {
    return make_document(
        kvp("incarnation", record.incarnation),
        kvp("draining", record.draining));
}
bsoncxx::document::value shard_identity(
    std::uint32_t realm, std::uint64_t shard) {
    if (realm == 0) throw PlayerDataError("invalid shard realm");
    return make_document(
        kvp("realm_id", std::int64_t{realm}),
        kvp("shard_id", database_id(shard, "shard_id")));
}
bsoncxx::document::value shard_fields(const SceneShardRecord& record) {
    bsoncxx::builder::basic::document fields;
    fields.append(
        kvp("version", database_counter(record.version, "shard_version")),
        kvp("draining", record.draining));
    if (record.owner)
        fields.append(
            kvp("owner",
                make_document(
                    kvp("instance_id", record.owner->instance_id),
                    kvp("incarnation", record.owner->incarnation))));
    else
        fields.append(kvp("owner", bsoncxx::types::b_null{}));
    return fields.extract();
}
SceneShardRecord shard_from(
    std::uint32_t realm, std::uint64_t shard, bsoncxx::document::view view) {
    SceneShardRecord record{
        realm,
        shard,
        domain_counter(view["version"]),
        std::nullopt,
        domain_flag(view["draining"])};
    if (record.version == 0) throw PlayerDataError("invalid shard version");
    if (view["owner"] && view["owner"].type() != bsoncxx::type::k_null) {
        const auto owner = scene_document(view["owner"]);
        record.owner = SceneShardOwner{
            domain_string(owner["instance_id"]),
            domain_string(owner["incarnation"])};
        if (!valid_scene_text(record.owner->instance_id) ||
            !valid_scene_text(record.owner->incarnation))
            throw PlayerDataError("invalid shard owner");
    }
    if (!record.owner && record.draining)
        throw PlayerDataError("released shard cannot be draining");
    return record;
}
void validate_shard_cas(
    const std::optional<SceneShardRecord>& expected,
    const SceneShardRecord& desired) {
    static_cast<void>(shard_identity(desired.realm_id, desired.shard_id));
    if (desired.version == 0 || desired.version > scene_counter_max ||
        (!desired.owner && desired.draining) ||
        (desired.owner && (!valid_scene_text(desired.owner->instance_id) ||
                           !valid_scene_text(desired.owner->incarnation))))
        throw PlayerDataError("invalid shard CAS");
    if (!expected) {
        if (desired.version != 1 || !desired.owner || desired.draining)
            throw PlayerDataError("invalid first shard owner");
        return;
    }
    if (expected->realm_id != desired.realm_id ||
        expected->shard_id != desired.shard_id || expected->version == 0)
        throw PlayerDataError("shard CAS key mismatch");
    const bool changed = expected->owner != desired.owner;
    if ((changed && expected->version == scene_counter_max) ||
        desired.version != expected->version + (changed ? 1U : 0U) ||
        (!changed && expected->draining && !desired.draining) ||
        (changed && desired.draining) ||
        (changed && !desired.owner && !expected->draining))
        throw PlayerDataError("invalid shard ownership transition");
}
}  // namespace

class MongoPlayerDataStore::Impl final {
public:
    Impl(std::string uri, std::string database, MongoPlayerDataOptions options)
        : database_name_(std::move(database)),
          write_timeout_(options.socket_timeout),
          credential_hash_cost_(options.credential_hash_cost) {
        if (database_name_.empty()) {
            throw PlayerDataError("player data database must not be empty");
        }
        if (sodium_init() < 0) {
            throw PlayerDataError("failed to initialize credential hashing");
        }
        ensure_driver_instance();
        translate_driver_errors([&] {
            pool_.emplace(configured_uri(std::move(uri), options));
            auto client = pool_->acquire();
            require_replica_set(*client);
            ensure_schema(*client);
        });
        if (options.bootstrap_accounts_file.has_value()) {
            static_cast<void>(
                bootstrap_from_lua(*options.bootstrap_accounts_file));
        }
    }

    [[nodiscard]] std::optional<AccountRecord> authenticate(
        std::string_view account, std::string_view credential) const {
        std::optional<AccountRecord> record;
        std::string hash;
        translate_driver_errors([&] {
            auto client = pool_->acquire();
            const auto stored = collection(*client, accounts_collection)
                                    .find_one(make_document(
                                        kvp("account_name", text(account))));
            if (!stored.has_value()) return;
            const auto view = stored->view();
            hash = domain_string(view["credential_hash"]);
            record = AccountRecord{
                .account_id = domain_id(view["_id"]),
                .banned = domain_flag(view["banned"]),
                .whitelisted = domain_flag(view["whitelisted"]),
            };
        });
        // Argon2 校验刻意耗时且占内存，放在连接归还连接池之后执行，避免
        // 把同一进程的其他查询(含 Realm/Gateway 读路径)挡在口令校验之后。
        if (!record.has_value() || !credential_matches(hash, credential)) {
            return std::nullopt;
        }
        return record;
    }

    [[nodiscard]] std::optional<AccountLoginFacts> login_facts(
        std::uint64_t account_id) const {
        const auto id = database_id(account_id, "account_id");
        return translate_driver_errors(
            [&]() -> std::optional<AccountLoginFacts> {
                auto client = pool_->acquire();
                mongocxx::options::find id_only;
                id_only.projection(make_document(kvp("_id", 1)));
                const auto account =
                    collection(*client, accounts_collection)
                        .find_one(
                            make_document(
                                kvp("_id", id),
                                kvp("banned", false),
                                kvp("whitelisted", true)),
                            id_only);
                if (!account.has_value()) return std::nullopt;
                return AccountLoginFacts{.account_id = account_id};
            });
    }

    [[nodiscard]] CharacterRoster list_characters(
        std::uint64_t account_id, std::uint32_t realm_id) const {
        const auto account = database_id(account_id, "account_id");
        const auto realm = database_realm_id(realm_id);
        return translate_driver_errors([&] {
            auto client = pool_->acquire();
            CharacterRoster roster;
            // roster_seq 是账号内的创建序;导入/开通的角色没有它,排在前面。
            mongocxx::options::find ordered;
            ordered.sort(make_document(kvp("roster_seq", 1), kvp("_id", 1)));
            auto cursor = collection(*client, characters_collection)
                              .find(
                                  make_document(
                                      kvp("account_id", account),
                                      kvp("realm_id", realm)),
                                  ordered);
            for (const auto& document : cursor) {
                roster.characters.push_back(realm_character_from(document));
            }

            mongocxx::options::find selected_only;
            selected_only.projection(
                make_document(kvp("selected_character_id", 1)));
            const auto stored = collection(*client, accounts_collection)
                                    .find_one(
                                        make_document(kvp("_id", account)),
                                        selected_only);
            if (!stored.has_value()) return roster;
            const auto selected = stored->view()["selected_character_id"];
            if (!selected || selected.type() == bsoncxx::type::k_null) {
                return roster;
            }
            const auto selected_id = domain_id(selected);
            if (std::ranges::any_of(
                    roster.characters, [&](const RealmCharacter& character) {
                        return character.character_id == selected_id;
                    })) {
                roster.last_selected_character_id = selected_id;
            }
            return roster;
        });
    }

    [[nodiscard]] CreateCharacterResult create_character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::string_view name,
        std::size_t max_characters) {
        const auto account = database_id(account_id, "account_id");
        const auto realm = database_realm_id(realm_id);
        if (name.empty() || max_characters == 0) {
            throw PlayerDataError("character name and limit must be non-empty");
        }
        const auto limit = static_cast<std::int64_t>(max_characters);
        for (std::size_t attempt = 0; attempt < character_id_attempts;
             ++attempt) {
            const auto character_id = random_character_id();
            const auto outcome = translate_driver_errors(
                [&]() -> std::optional<CreateCharacterResult> {
                    auto client = pool_->acquire();
                    auto session = client->start_session();
                    std::optional<CreateCharacterResult> result;
                    bool missing_account = false;
                    try {
                        session.with_transaction(
                            [&](mongocxx::client_session* transaction) {
                                result.reset();
                                missing_account = false;
                                // 先写账号文档:同账号的并发创建在此写冲突,
                                // 事务重试后看到对方已提交的角色再计数。
                                mongocxx::options::find_one_and_update after;
                                after.return_document(
                                    mongocxx::options::return_document::k_after);
                                after.projection(
                                    make_document(kvp("roster_seq", 1)));
                                const auto stored =
                                    collection(*client, accounts_collection)
                                        .find_one_and_update(
                                            *transaction,
                                            make_document(kvp("_id", account)),
                                            make_document(kvp(
                                                "$inc",
                                                make_document(kvp(
                                                    "roster_seq",
                                                    std::int64_t{1})))),
                                            after);
                                if (!stored.has_value()) {
                                    missing_account = true;
                                    return;
                                }
                                const auto roster_seq =
                                    stored->view()["roster_seq"].get_int64();
                                const auto count =
                                    collection(*client, characters_collection)
                                        .count_documents(
                                            *transaction,
                                            make_document(
                                                kvp("account_id", account),
                                                kvp("realm_id", realm)));
                                if (count >= limit) {
                                    result = CreateCharacterResult{
                                        .outcome =
                                            CreateCharacterOutcome::LimitReached};
                                    return;
                                }
                                static_cast<void>(
                                    collection(*client, characters_collection)
                                        .insert_one(
                                            *transaction,
                                            make_document(
                                                kvp("_id",
                                                    database_id(
                                                        character_id,
                                                        "character_id")),
                                                kvp("account_id", account),
                                                kvp("realm_id", realm),
                                                kvp("name", text(name)),
                                                kvp("exp", std::int64_t{0}),
                                                kvp("last_training_seq",
                                                    std::int64_t{0}),
                                                kvp("revision", std::int64_t{1}),
                                                kvp("roster_seq", roster_seq))));
                                result = CreateCharacterResult{
                                    .outcome = CreateCharacterOutcome::Created,
                                    .character = RealmCharacter{
                                        .character_id = character_id,
                                        .name = std::string(name),
                                    }};
                            },
                            transaction_options());
                    } catch (const mongocxx::operation_exception& error) {
                        if (!is_duplicate_key(error)) throw;
                        // 撞名(唯一索引)或撞号(_id):库中已有同名即撞名,
                        // 否则换一个编号重试。
                        if (collection(*client, characters_collection)
                                .find_one(make_document(
                                    kvp("realm_id", realm),
                                    kvp("name", text(name))))
                                .has_value()) {
                            return CreateCharacterResult{
                                .outcome = CreateCharacterOutcome::NameTaken};
                        }
                        return std::nullopt;
                    }
                    if (missing_account) {
                        throw PlayerDataError("account does not exist");
                    }
                    return result;
                });
            if (outcome.has_value()) return *outcome;
        }
        throw PlayerDataError("failed to allocate a unique character id");
    }

    [[nodiscard]] std::optional<RealmCharacter> choose_character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) {
        const auto account = database_id(account_id, "account_id");
        const auto character = realm_character(account_id, realm_id, character_id);
        if (!character.has_value()) return std::nullopt;
        // 角色既不删除也不改派:归属一经确认,随后写账号没有竞态窗口。
        translate_driver_errors([&] {
            auto client = pool_->acquire();
            const auto result =
                collection(*client, accounts_collection)
                    .update_one(
                        make_document(kvp("_id", account)),
                        make_document(kvp(
                            "$set",
                            make_document(kvp(
                                "selected_character_id",
                                database_id(character_id, "character_id"))))));
            if (!result.has_value() || result->matched_count() != 1) {
                throw PlayerDataError("account does not exist");
            }
        });
        return character;
    }

    [[nodiscard]] std::optional<RealmCharacter> realm_character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) const {
        const auto filter = owned_character_filter(
            account_id, realm_id, character_id);
        return translate_driver_errors([&]() -> std::optional<RealmCharacter> {
            auto client = pool_->acquire();
            const auto stored = collection(*client, characters_collection)
                                    .find_one(filter.view());
            if (!stored.has_value()) return std::nullopt;
            return realm_character_from(stored->view());
        });
    }

    [[nodiscard]] std::optional<TrainingWriteResult> record_training(
        const TrainingWrite& write) {
        if (write.seq == 0) {
            throw PlayerDataError("training seq must be positive");
        }
        const auto filter = make_document(
            kvp("_id", database_id(write.character_id, "character_id")),
            kvp("account_id", database_id(write.account_id, "account_id")),
            kvp("realm_id", database_realm_id(write.realm_id)),
            kvp("last_training_seq",
                database_counter(write.seq - 1, "last_training_seq")));
        const auto update = make_document(
            kvp("$set",
                make_document(
                    kvp("exp", database_counter(write.exp, "exp")),
                    kvp("last_training_seq",
                        database_counter(write.seq, "last_training_seq")))),
            kvp("$inc", make_document(kvp("revision", std::int64_t{1}))));
        const auto committed = translate_driver_errors(
            [&]() -> std::optional<RealmCharacter> {
                auto client = pool_->acquire();
                mongocxx::options::find_one_and_update after;
                after.return_document(
                    mongocxx::options::return_document::k_after);
                const auto stored =
                    collection(*client, characters_collection)
                        .find_one_and_update(filter.view(), update.view(), after);
                if (!stored.has_value()) return std::nullopt;
                return realm_character_from(stored->view());
            });
        if (committed.has_value()) {
            return TrainingWriteResult{.committed = true, .character = *committed};
        }
        const auto current = realm_character(
            write.account_id, write.realm_id, write.character_id);
        if (!current.has_value()) return std::nullopt;
        return TrainingWriteResult{.committed = false, .character = *current};
    }

    [[nodiscard]] std::optional<SceneStateRecord> scene_state(
        const SceneCharacterKey& key) const {
        const auto filter = scene_filter(key);
        if (!realm_character(key.account_id, key.realm_id, key.character_id))
            return std::nullopt;
        return translate_driver_errors(
            [&]() -> std::optional<SceneStateRecord> {
                auto client = pool_->acquire();
                const auto stored = collection(*client, scenes_collection)
                                        .find_one(filter.view());
                if (!stored) return std::nullopt;
                return scene_state_from(key, stored->view());
            });
    }

    [[nodiscard]] std::optional<SceneStateRecord> initialize_scene(
        const SceneCharacterKey& key) {
        const auto filter = scene_filter(key);
        if (!realm_character(key.account_id, key.realm_id, key.character_id))
            return std::nullopt;
        return translate_driver_errors(
            [&]() -> std::optional<SceneStateRecord> {
                auto client = pool_->acquire();
                mongocxx::options::find_one_and_update after;
                after.upsert(true);
                after.return_document(
                    mongocxx::options::return_document::k_after);
                try {
                    const auto stored =
                        collection(*client, scenes_collection)
                            .find_one_and_update(
                                filter.view(),
                                make_document(kvp(
                                    "$setOnInsert",
                                    scene_fields(
                                        SceneStateRecord{.character = key}))),
                                after);
                    if (!stored)
                        throw PlayerDataError(
                            "scene initialization returned no state");
                    return scene_state_from(key, stored->view());
                } catch (const mongocxx::operation_exception& error) {
                    if (!is_duplicate_key(error)) throw;
                    // 并发惰性初始化只创建一条记录；回读获胜者，不能覆盖已有状态。
                    const auto stored = collection(*client, scenes_collection)
                                            .find_one(filter.view());
                    if (!stored)
                        throw PlayerDataError(
                            "scene initialization ownership conflict");
                    return scene_state_from(key, stored->view());
                }
            });
    }

    [[nodiscard]] std::optional<SceneInstanceState> scene_instance_state(
        const std::string& id) const {
        if (!valid_scene_text(id))
            throw PlayerDataError("invalid Scene instance");
        return translate_driver_errors(
            [&]() -> std::optional<SceneInstanceState> {
                auto client = pool_->acquire();
                const auto stored =
                    collection(*client, scene_instances_collection)
                        .find_one(make_document(kvp("_id", id)));
                if (!stored) return std::nullopt;
                const auto view = stored->view();
                SceneInstanceState value{
                    id,
                    domain_string(view["incarnation"]),
                    domain_flag(view["draining"])};
                if (!valid_scene_text(value.incarnation))
                    throw PlayerDataError("invalid Scene incarnation");
                return value;
            });
    }
    [[nodiscard]] bool compare_exchange_scene_instance(
        const std::optional<SceneInstanceState>& expected,
        const SceneInstanceState& desired) {
        if (!valid_scene_text(desired.instance_id) ||
            !valid_scene_text(desired.incarnation) ||
            (!expected && desired.draining) ||
            (expected && (expected->instance_id != desired.instance_id ||
                          (expected->incarnation == desired.incarnation &&
                           expected->draining && !desired.draining))))
            throw PlayerDataError("invalid Scene instance CAS");
        bool matched = false;
        try {
            auto client = pool_->acquire();
            auto session = client->start_session();
            session.with_transaction(
                [&](mongocxx::client_session* tx) {
                    matched = false;
                    auto instances =
                        collection(*client, scene_instances_collection);
                    bsoncxx::builder::basic::document filter;
                    filter.append(kvp("_id", desired.instance_id));
                    if (!expected) {
                        filter.append(
                            bsoncxx::builder::basic::concatenate(
                                instance_fields(desired).view()));
                        static_cast<void>(
                            instances.insert_one(*tx, filter.view()));
                        matched = true;
                    } else {
                        filter.append(
                            bsoncxx::builder::basic::concatenate(
                                instance_fields(*expected).view()));
                        const auto result = instances.update_one(
                            *tx,
                            filter.view(),
                            make_document(
                                kvp("$set", instance_fields(desired))));
                        matched = result && result->matched_count() == 1;
                    }
                    if (matched && desired.draining)
                        static_cast<void>(
                            collection(*client, shards_collection)
                                .update_many(
                                    *tx,
                                    make_document(
                                        kvp("owner.instance_id",
                                            desired.instance_id),
                                        kvp("owner.incarnation",
                                            desired.incarnation)),
                                    make_document(
                                        kvp("$set",
                                            make_document(
                                                kvp("draining", true))))));
                },
                transaction_options());
            return matched;
        } catch (const mongocxx::operation_exception& error) {
            if (!expected && error.code().value() == duplicate_key_code)
                return false;
            throw SceneWriteUncertain(
                std::string("Scene instance CAS outcome unknown: ") +
                error.what());
        } catch (const mongocxx::exception& error) {
            throw SceneWriteUncertain(
                std::string("Scene instance CAS outcome unknown: ") +
                error.what());
        }
    }
    [[nodiscard]] std::optional<SceneShardRecord> shard_state(
        std::uint32_t realm, std::uint64_t shard) const {
        const auto identity = shard_identity(realm, shard);
        return translate_driver_errors(
            [&]() -> std::optional<SceneShardRecord> {
                auto client = pool_->acquire();
                const auto stored =
                    collection(*client, shards_collection)
                        .find_one(make_document(kvp("_id", identity)));
                if (!stored) return std::nullopt;
                return shard_from(realm, shard, stored->view());
            });
    }
    [[nodiscard]] bool compare_exchange_shard(
        const std::optional<SceneShardRecord>& expected,
        const SceneShardRecord& desired) {
        validate_shard_cas(expected, desired);
        const auto identity =
            shard_identity(desired.realm_id, desired.shard_id);
        try {
            auto client = pool_->acquire();
            auto session = client->start_session();
            bool matched = false;
            session.with_transaction(
                [&](mongocxx::client_session* tx) {
                    matched = false;
                    if (desired.owner &&
                        (!expected || expected->owner != desired.owner)) {
                        const auto ready =
                            collection(*client, scene_instances_collection)
                                .update_one(
                                    *tx,
                                    make_document(
                                        kvp("_id", desired.owner->instance_id),
                                        kvp("incarnation",
                                            desired.owner->incarnation),
                                        kvp("draining", false)),
                                    make_document(
                                        kvp("$inc",
                                            make_document(
                                                kvp("assignment_guard",
                                                    std::int64_t{1})))));
                        if (!ready || ready->matched_count() != 1) return;
                    }
                    auto shards = collection(*client, shards_collection);
                    if (!expected) {
                        bsoncxx::builder::basic::document value;
                        value.append(
                            kvp("_id", identity),
                            bsoncxx::builder::basic::concatenate(
                                shard_fields(desired).view()));
                        static_cast<void>(shards.insert_one(*tx, value.view()));
                        matched = true;
                        return;
                    }
                    bsoncxx::builder::basic::document filter;
                    filter.append(
                        kvp("_id", identity),
                        bsoncxx::builder::basic::concatenate(
                            shard_fields(*expected).view()));
                    const auto result = shards.update_one(
                        *tx,
                        filter.view(),
                        make_document(kvp("$set", shard_fields(desired))));
                    matched = result && result->matched_count() == 1;
                },
                transaction_options());
            return matched;
        } catch (const mongocxx::operation_exception& error) {
            if (!expected && error.code().value() == duplicate_key_code)
                return false;
            throw SceneWriteUncertain(
                std::string("shard CAS outcome unknown: ") + error.what());
        } catch (const mongocxx::exception& error) {
            throw SceneWriteUncertain(
                std::string("shard CAS outcome unknown: ") + error.what());
        }
    }
    bool guard_shard(
        mongocxx::client& client,
        mongocxx::client_session& transaction,
        std::uint32_t realm,
        std::uint64_t shard,
        const SceneOwner& owner,
        bool admitting) {
        bsoncxx::builder::basic::document filter;
        filter.append(
            kvp("_id", shard_identity(realm, shard)),
            kvp("version",
                database_counter(owner.shard_version, "shard_version")),
            kvp("owner.instance_id", owner.scene_instance_id),
            kvp("owner.incarnation", owner.scene_incarnation));
        if (admitting) filter.append(kvp("draining", false));
        // 写栅栏与承载 CAS 建立写冲突；仅事务快照读不能阻止写偏斜。
        const auto matched =
            collection(client, shards_collection)
                .update_one(
                    transaction,
                    filter.view(),
                    make_document(kvp(
                        "$inc",
                        make_document(kvp("commit_guard", std::int64_t{1})))));
        return matched && matched->matched_count() == 1;
    }
    [[nodiscard]] SceneWriteResult commit_scene(const SceneWrite& write) {
        if (!valid_scene_owner(write.owner) ||
            (write.expected_owner &&
             !valid_scene_owner(*write.expected_owner)) ||
            !valid_scene_location(write.location) ||
            !valid_scene_command(write.command) ||
            write.expected_epoch > scene_counter_max) {
            throw PlayerDataError("invalid scene write");
        }
        auto current = scene_state(write.character);
        if (!current) current = initialize_scene(write.character);
        if (!current) return {.outcome = SceneWriteOutcome::NotOwned};
        const auto decision = check_scene_transition(*current, write);
        if (decision != SceneWriteOutcome::Committed)
            return {decision, std::move(current)};
        const auto epoch =
            write.expected_epoch +
            (write.command.type == SceneCommandType::Move ? 0U : 1U);
        SceneStateRecord next{
            .character = write.character,
            .binding_epoch = epoch,
            .owner = write.owner,
            .confirmed_location = write.location,
            .last_result = SceneStoredResult{
                write.command, write.location, epoch, write.owner}};
        bsoncxx::builder::basic::document filter;
        filter.append(
            bsoncxx::builder::basic::concatenate(
                scene_cas_filter(
                    write.character, write.expected_epoch, write.expected_owner)
                    .view()),
            kvp("last_scene_seq",
                database_counter(write.command.seq - 1, "scene_seq")));
        const auto update = make_document(kvp("$set", scene_fields(next)));
        try {
            auto client = pool_->acquire();
            mongocxx::options::find_one_and_update after;
            after.return_document(mongocxx::options::return_document::k_after);
            decltype(collection(*client, scenes_collection)
                         .find_one(filter.view())) stored;
            bool stale_shard = false;
            if (write.owner.shard_version != 0) {
                auto transaction = client->start_session();
                transaction.with_transaction(
                    [&](mongocxx::client_session* session) {
                        stored.reset();
                        stale_shard = !guard_shard(
                            *client,
                            *session,
                            write.character.realm_id,
                            write.location.shard_id,
                            write.owner,
                            write.command.type != SceneCommandType::Move);
                        if (!stale_shard &&
                            write.command.type == SceneCommandType::Transfer &&
                            write.expected_owner &&
                            write.expected_owner->shard_version != 0)
                            stale_shard = !guard_shard(
                                *client,
                                *session,
                                write.character.realm_id,
                                current->confirmed_location->shard_id,
                                *write.expected_owner,
                                false);
                        if (stale_shard) return;
                        stored = collection(*client, scenes_collection)
                                     .find_one_and_update(
                                         *session,
                                         filter.view(),
                                         update.view(),
                                         after);
                    },
                    transaction_options());
            } else {
                stored = collection(*client, scenes_collection)
                             .find_one_and_update(
                                 filter.view(), update.view(), after);
            }
            if (stale_shard)
                return {SceneWriteOutcome::StaleBinding, std::move(current)};
            if (stored)
                return {
                    SceneWriteOutcome::Committed,
                    scene_state_from(write.character, stored->view())};
        } catch (const mongocxx::exception& error) {
            throw SceneWriteUncertain(
                std::string("scene commit outcome unknown: ") + error.what());
        } catch (const bsoncxx::exception& error) {
            throw SceneWriteUncertain(
                std::string("scene commit result unreadable: ") + error.what());
        } catch (const PlayerDataError& error) {
            throw SceneWriteUncertain(
                std::string("scene committed result invalid: ") + error.what());
        }
        // CAS
        // 明确未命中；只接受精确回放，绝不根据一次旧值回读推断另一笔超时写未提交。
        current = scene_state(write.character);
        if (!current) return {.outcome = SceneWriteOutcome::NotOwned};
        const auto conflict = check_scene_transition(*current, write);
        if (conflict == SceneWriteOutcome::Committed)
            return {SceneWriteOutcome::StaleBinding, std::move(current)};
        return {conflict, std::move(current)};
    }

    [[nodiscard]] bool revoke_scene(
        const SceneCharacterKey& key,
        std::uint64_t epoch,
        const SceneOwner& owner) {
        if (epoch == 0 || epoch >= scene_counter_max ||
            !valid_scene_owner(owner))
            throw PlayerDataError("invalid scene revocation");
        const auto filter = scene_cas_filter(key, epoch, owner);
        if (!realm_character(key.account_id, key.realm_id, key.character_id))
            return false;
        const auto update = make_document(
            kvp("$set", make_document(kvp("owner", bsoncxx::types::b_null{}))),
            kvp("$inc", make_document(kvp("binding_epoch", std::int64_t{1}))));
        try {
            auto client = pool_->acquire();
            const auto result = collection(*client, scenes_collection)
                                    .update_one(filter.view(), update.view());
            return result && result->matched_count() == 1;
        } catch (const mongocxx::exception& error) {
            throw SceneWriteUncertain(
                std::string("scene revoke outcome unknown: ") + error.what());
        } catch (const bsoncxx::exception& error) {
            throw SceneWriteUncertain(
                std::string("scene revoke result unreadable: ") + error.what());
        }
    }

    void provision_account(const AccountProvisioning& account) {
        if (account.account_name.empty()) {
            throw PlayerDataError("account name must not be empty");
        }
        const auto id = database_id(account.account_id, "account_id");
        const auto hash =
            credential_hash(account.credential, credential_hash_cost_);
        translate_driver_errors([&] {
            auto client = pool_->acquire();
            mongocxx::options::update upsert;
            upsert.upsert(true);
            // 只覆盖认证与准入字段；已选角色不受重新开通影响。
            try {
                static_cast<void>(
                    collection(*client, accounts_collection)
                        .update_one(
                            make_document(kvp("_id", id)),
                            make_document(kvp(
                                "$set",
                                make_document(
                                    kvp("account_name",
                                        text(account.account_name)),
                                    kvp("credential_hash", text(hash)),
                                    kvp("banned", account.banned),
                                    kvp("whitelisted", account.whitelisted)))),
                            upsert));
            } catch (const mongocxx::operation_exception& error) {
                if (is_duplicate_key(error)) {
                    throw PlayerDataError("account name is already taken");
                }
                throw;
            }
        });
    }

    void set_account_access(
        std::uint64_t account_id, bool banned, bool whitelisted) {
        const auto id = database_id(account_id, "account_id");
        translate_driver_errors([&] {
            auto client = pool_->acquire();
            const auto result =
                collection(*client, accounts_collection)
                    .update_one(
                        make_document(kvp("_id", id)),
                        make_document(kvp(
                            "$set",
                            make_document(
                                kvp("banned", banned),
                                kvp("whitelisted", whitelisted)))));
            if (!result.has_value() || result->matched_count() != 1) {
                throw PlayerDataError("account does not exist");
            }
        });
    }

    void provision_character(const CharacterRecord& character) {
        if (character.realm_id == 0 || character.name.empty() ||
            character.revision == 0) {
            throw PlayerDataError("character fields must be non-empty");
        }
        const auto character_id =
            database_id(character.character_id, "character_id");
        const auto account_id =
            database_id(character.account_id, "account_id");
        const auto revision = database_id(character.revision, "revision");
        translate_driver_errors([&] {
            auto client = pool_->acquire();
            mongocxx::options::find id_only;
            id_only.projection(make_document(kvp("_id", 1)));
            if (!collection(*client, accounts_collection)
                     .find_one(make_document(kvp("_id", account_id)), id_only)
                     .has_value()) {
                throw PlayerDataError("account does not exist");
            }
            // 归属字段放进过滤条件：同一编号已归属别的账号或 Realm 时不匹配，
            // upsert 转为插入并撞上 _id 唯一约束，从而拒绝改派。
            mongocxx::options::update upsert;
            upsert.upsert(true);
            try {
                static_cast<void>(
                    collection(*client, characters_collection)
                        .update_one(
                            make_document(
                                kvp("_id", character_id),
                                kvp("account_id", account_id),
                                kvp("realm_id",
                                    std::int64_t{character.realm_id})),
                            make_document(
                                kvp("$set",
                                    make_document(
                                        kvp("name", text(character.name)),
                                        kvp("revision", revision))),
                                kvp("$setOnInsert",
                                    make_document(
                                        kvp("exp", std::int64_t{0}),
                                        kvp("last_training_seq",
                                            std::int64_t{0})))),
                            upsert));
            } catch (const mongocxx::operation_exception& error) {
                if (is_duplicate_key(error)) {
                    throw PlayerDataError(
                        "character ownership cannot be reassigned");
                }
                throw;
            }
        });
    }

    void select_character(
        std::uint64_t account_id, std::uint64_t character_id) {
        const auto account = database_id(account_id, "account_id");
        const auto character = database_id(character_id, "character_id");
        translate_driver_errors([&] {
            auto client = pool_->acquire();
            // 角色既不删除也不改派，先确认归属再写账号即无竞态窗口。
            mongocxx::options::find id_only;
            id_only.projection(make_document(kvp("_id", 1)));
            const auto owned =
                collection(*client, characters_collection)
                    .find_one(
                        make_document(
                            kvp("_id", character), kvp("account_id", account)),
                        id_only);
            if (!owned.has_value()) {
                throw PlayerDataError(
                    "selected character must belong to the account");
            }
            const auto result =
                collection(*client, accounts_collection)
                    .update_one(
                        make_document(kvp("_id", account)),
                        make_document(kvp(
                            "$set",
                            make_document(kvp(
                                "selected_character_id", character)))));
            if (!result.has_value() || result->matched_count() != 1) {
                throw PlayerDataError("account does not exist");
            }
        });
    }

    [[nodiscard]] bool bootstrap_from_lua(
        const std::filesystem::path& accounts_file) {
        // 每个服务启动都会进入这里;先做廉价检查，已导入的库不再读取与哈希
        // Lua 账号。
        const bool pending = translate_driver_errors([&] {
            auto client = pool_->acquire();
            return bootstrap_pending(*client, nullptr);
        });
        if (!pending) return false;

        const auto accounts =
            load_bootstrap_accounts(accounts_file, credential_hash_cost_);
        std::vector<bsoncxx::document::value> account_documents;
        std::vector<bsoncxx::document::value> character_documents;
        account_documents.reserve(accounts.size());
        character_documents.reserve(accounts.size());
        for (const auto& entry : accounts) {
            const auto id = database_id(entry.account.account_id, "account_id");
            account_documents.push_back(make_document(
                kvp("_id", id),
                kvp("account_name", text(entry.account.account_name)),
                kvp("credential_hash", text(entry.hash)),
                kvp("banned", entry.account.banned),
                kvp("whitelisted", entry.account.whitelisted),
                kvp("selected_character_id", id)));
            character_documents.push_back(make_document(
                kvp("_id", id),
                kvp("account_id", id),
                kvp("realm_id", std::int64_t{1}),
                kvp("name", text(entry.account.account_name)),
                kvp("exp", std::int64_t{0}),
                kvp("last_training_seq", std::int64_t{0}),
                kvp("revision", std::int64_t{1})));
        }

        return translate_driver_errors([&] {
            auto client = pool_->acquire();
            auto session = client->start_session();
            bool imported = false;
            try {
                session.with_transaction(
                    [&](mongocxx::client_session* transaction) {
                        imported = false;
                        // 哈希期间另一服务可能已完成导入，事务内重新确认。
                        if (!bootstrap_pending(*client, transaction)) return;
                        // 先写标记：并发导入者在这里写冲突，with_transaction
                        // 重试时就能看到已提交的标记而退出。
                        static_cast<void>(
                            collection(*client, metadata_collection)
                                .insert_one(
                                    *transaction,
                                    make_document(
                                        kvp("_id", text(bootstrap_marker)),
                                        kvp("value", "complete"))));
                        if (!account_documents.empty()) {
                            static_cast<void>(
                                collection(*client, accounts_collection)
                                    .insert_many(
                                        *transaction, account_documents));
                            static_cast<void>(
                                collection(*client, characters_collection)
                                    .insert_many(
                                        *transaction, character_documents));
                        }
                        imported = true;
                    },
                    transaction_options());
            } catch (const mongocxx::operation_exception& error) {
                // 对方已提交的标记撞主键不是瞬时错误，不会被自动重试。
                if (!is_duplicate_key(error) ||
                    bootstrap_pending(*client, nullptr)) {
                    throw;
                }
                return false;
            }
            return imported;
        });
    }

private:
    [[nodiscard]] static std::int64_t database_realm_id(std::uint32_t realm_id) {
        if (realm_id == 0) throw PlayerDataError("realm_id must be non-zero");
        return std::int64_t{realm_id};
    }

    [[nodiscard]] static bsoncxx::document::value owned_character_filter(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) {
        return make_document(
            kvp("_id", database_id(character_id, "character_id")),
            kvp("account_id", database_id(account_id, "account_id")),
            kvp("realm_id", database_realm_id(realm_id)));
    }

    [[nodiscard]] mongocxx::collection collection(
        mongocxx::client& client, std::string_view name) const {
        auto database = client[database_name_];
        database.write_concern(durable_write_concern());
        database.read_concern(majority_read_concern());
        return database[std::string(name)];
    }

    [[nodiscard]] mongocxx::write_concern durable_write_concern() const {
        mongocxx::write_concern concern;
        concern.majority(write_timeout_);
        concern.journal(true);
        return concern;
    }

    [[nodiscard]] static mongocxx::read_concern majority_read_concern() {
        mongocxx::read_concern concern;
        concern.acknowledge_level(mongocxx::read_concern::level::k_majority);
        return concern;
    }

    [[nodiscard]] mongocxx::options::transaction transaction_options() const {
        mongocxx::options::transaction options;
        options.write_concern(durable_write_concern());
        options.read_concern(majority_read_concern());
        return options;
    }

    static void require_replica_set(mongocxx::client& client) {
        const auto hello =
            client["admin"].run_command(make_document(kvp("hello", 1)));
        if (!hello.view()["setName"]) {
            throw PlayerDataError(
                "player data requires a MongoDB replica set (ADR-0011)");
        }
    }

    void ensure_schema(mongocxx::client& client) const {
        mongocxx::options::index unique;
        unique.unique(true);
        static_cast<void>(collection(client, accounts_collection)
                              .create_index(
                                  make_document(kvp("account_name", 1)),
                                  unique));
        static_cast<void>(collection(client, characters_collection)
                              .create_index(make_document(
                                  kvp("account_id", 1), kvp("realm_id", 1))));
        // 角色名在 Realm 内按字节精确唯一(#93);库中已有重名时建索引失败,
        // 服务拒绝启动,由运维先清理。
        static_cast<void>(collection(client, characters_collection)
                              .create_index(
                                  make_document(
                                      kvp("realm_id", 1), kvp("name", 1)),
                                  unique));

        mongocxx::options::find_one_and_update upsert;
        upsert.upsert(true);
        upsert.return_document(mongocxx::options::return_document::k_after);
        const auto schema =
            collection(client, metadata_collection)
                .find_one_and_update(
                    make_document(kvp("_id", text(schema_marker))),
                    make_document(kvp(
                        "$setOnInsert",
                        make_document(kvp("version", schema_version)))),
                    upsert);
        const auto version =
            schema.has_value() ? schema->view()["version"]
                               : bsoncxx::document::element{};
        if (!version || version.type() != bsoncxx::type::k_int64) {
            throw PlayerDataError("player data schema marker is invalid");
        }
        if (version.get_int64().value > schema_version) {
            throw PlayerDataError(
                "player data schema is newer than this binary");
        }
        if (version.get_int64().value < schema_version) migrate_to_v2(client);
    }

    /// schema 1 → 2:给已有角色补经验与训练序号。幂等,几个服务并发
    /// 启动时各执行一遍也只补一次。
    void migrate_to_v2(mongocxx::client& client) const {
        auto characters = collection(client, characters_collection);
        for (const std::string_view field : {"exp", "last_training_seq"}) {
            static_cast<void>(characters.update_many(
                make_document(
                    kvp(std::string(field), make_document(kvp("$exists", false)))),
                make_document(kvp(
                    "$set", make_document(kvp(std::string(field), std::int64_t{0}))))));
        }
        static_cast<void>(
            collection(client, metadata_collection)
                .update_one(
                    make_document(
                        kvp("_id", text(schema_marker)),
                        kvp("version",
                            make_document(kvp("$lt", schema_version)))),
                    make_document(kvp(
                        "$set",
                        make_document(kvp("version", schema_version))))));
    }

    [[nodiscard]] bool bootstrap_pending(
        mongocxx::client& client,
        mongocxx::client_session* transaction) const {
        const auto marker_filter =
            make_document(kvp("_id", text(bootstrap_marker)));
        auto metadata = collection(client, metadata_collection);
        const auto marker = transaction == nullptr
                                ? metadata.find_one(marker_filter.view())
                                : metadata.find_one(
                                      *transaction, marker_filter.view());
        if (marker.has_value()) return false;
        auto accounts = collection(client, accounts_collection);
        const auto any_account =
            transaction == nullptr
                ? accounts.find_one(make_document())
                : accounts.find_one(*transaction, make_document());
        return !any_account.has_value();
    }

    std::string database_name_;
    std::chrono::milliseconds write_timeout_;
    CredentialHashCost credential_hash_cost_{CredentialHashCost::Interactive};
    // mongocxx::pool 本身线程安全；可变状态只有它。
    mutable std::optional<mongocxx::pool> pool_;
};

MongoPlayerDataStore::MongoPlayerDataStore(
    std::string uri, std::string database, MongoPlayerDataOptions options)
    : impl_(std::make_unique<Impl>(
          std::move(uri), std::move(database), std::move(options))) {}

MongoPlayerDataStore::~MongoPlayerDataStore() = default;

std::optional<AccountRecord> MongoPlayerDataStore::authenticate(
    std::string_view account, std::string_view credential) const {
    return impl_->authenticate(account, credential);
}

std::optional<AccountLoginFacts> MongoPlayerDataStore::login_facts(
    std::uint64_t account_id) const {
    return impl_->login_facts(account_id);
}

CharacterRoster MongoPlayerDataStore::list_characters(
    std::uint64_t account_id, std::uint32_t realm_id) const {
    return impl_->list_characters(account_id, realm_id);
}

CreateCharacterResult MongoPlayerDataStore::create_character(
    std::uint64_t account_id,
    std::uint32_t realm_id,
    std::string_view name,
    std::size_t max_characters) {
    return impl_->create_character(account_id, realm_id, name, max_characters);
}

std::optional<RealmCharacter> MongoPlayerDataStore::choose_character(
    std::uint64_t account_id,
    std::uint32_t realm_id,
    std::uint64_t character_id) {
    return impl_->choose_character(account_id, realm_id, character_id);
}

std::optional<RealmCharacter> MongoPlayerDataStore::realm_character(
    std::uint64_t account_id,
    std::uint32_t realm_id,
    std::uint64_t character_id) const {
    return impl_->realm_character(account_id, realm_id, character_id);
}

std::optional<TrainingWriteResult> MongoPlayerDataStore::record_training(
    const TrainingWrite& write) {
    return impl_->record_training(write);
}

std::optional<SceneStateRecord> MongoPlayerDataStore::initialize_scene(
    const SceneCharacterKey& character) {
    return impl_->initialize_scene(character);
}
std::optional<SceneStateRecord> MongoPlayerDataStore::scene_state(
    const SceneCharacterKey& character) const {
    return impl_->scene_state(character);
}
SceneWriteResult MongoPlayerDataStore::commit_scene(const SceneWrite& write) {
    return impl_->commit_scene(write);
}
std::optional<SceneShardRecord> MongoPlayerDataStore::shard_state(
    std::uint32_t realm_id, std::uint64_t shard_id) const {
    return impl_->shard_state(realm_id, shard_id);
}
std::optional<SceneInstanceState> MongoPlayerDataStore::scene_instance_state(
    const std::string& instance_id) const {
    return impl_->scene_instance_state(instance_id);
}
bool MongoPlayerDataStore::compare_exchange_scene_instance(
    const std::optional<SceneInstanceState>& expected,
    const SceneInstanceState& desired) {
    return impl_->compare_exchange_scene_instance(expected, desired);
}
bool MongoPlayerDataStore::compare_exchange_shard(
    const std::optional<SceneShardRecord>& expected,
    const SceneShardRecord& desired) {
    return impl_->compare_exchange_shard(expected, desired);
}
bool MongoPlayerDataStore::revoke_scene(
    const SceneCharacterKey& character,
    std::uint64_t epoch,
    const SceneOwner& owner) {
    return impl_->revoke_scene(character, epoch, owner);
}

void MongoPlayerDataStore::provision_account(
    const AccountProvisioning& account) {
    impl_->provision_account(account);
}

void MongoPlayerDataStore::set_account_access(
    std::uint64_t account_id, bool banned, bool whitelisted) {
    impl_->set_account_access(account_id, banned, whitelisted);
}

void MongoPlayerDataStore::provision_character(
    const CharacterRecord& character) {
    impl_->provision_character(character);
}

void MongoPlayerDataStore::select_character(
    std::uint64_t account_id, std::uint64_t character_id) {
    impl_->select_character(account_id, character_id);
}

bool MongoPlayerDataStore::bootstrap_from_lua(
    const std::filesystem::path& accounts_file) {
    return impl_->bootstrap_from_lua(accounts_file);
}

}  // namespace realm::game::common
