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
