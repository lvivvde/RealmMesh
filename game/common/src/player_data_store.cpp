#include "realmmesh/game/common/player_data_store.hpp"

#include "realmmesh/scripting/lua_runtime.hpp"

#include <sodium.h>
#include <sqlite3.h>

#include <array>
#include <bit>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <unordered_set>
#include <vector>
#include <string>
#include <string_view>
#include <utility>

namespace realm::game::common {
namespace {

[[nodiscard]] sqlite3_int64 database_id(
    std::uint64_t value, std::string_view field) {
    if (value == 0) {
        throw PlayerDataError(std::string(field) + " must be non-zero");
    }
    static_assert(sizeof(sqlite3_int64) == sizeof(std::uint64_t));
    return std::bit_cast<sqlite3_int64>(value);
}

[[nodiscard]] std::uint64_t domain_id(sqlite3_int64 value) {
    if (value == 0) throw PlayerDataError("database contains an invalid id");
    static_assert(sizeof(sqlite3_int64) == sizeof(std::uint64_t));
    return std::bit_cast<std::uint64_t>(value);
}

[[nodiscard]] std::string sqlite_message(sqlite3* database, int status) {
    return "sqlite status " + std::to_string(status) + ": " +
           (database == nullptr ? "no database" : sqlite3_errmsg(database));
}

void execute(sqlite3* database, std::string_view sql) {
    char* message = nullptr;
    const int status = sqlite3_exec(
        database, std::string(sql).c_str(), nullptr, nullptr, &message);
    if (status == SQLITE_OK) return;
    const std::string detail =
        message == nullptr ? sqlite_message(database, status) : message;
    sqlite3_free(message);
    throw PlayerDataError(detail);
}

class Statement final {
public:
    Statement(sqlite3* database, std::string_view sql) : database_(database) {
        const int status = sqlite3_prepare_v2(
            database,
            sql.data(),
            static_cast<int>(sql.size()),
            &statement_,
            nullptr);
        if (status != SQLITE_OK) {
            throw PlayerDataError(sqlite_message(database, status));
        }
    }
    ~Statement() { sqlite3_finalize(statement_); }

    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    [[nodiscard]] sqlite3_stmt* get() const noexcept { return statement_; }

    void bind(std::string_view value, int index) {
        const int status = sqlite3_bind_text(
            statement_,
            index,
            value.data(),
            static_cast<int>(value.size()),
            SQLITE_TRANSIENT);
        if (status != SQLITE_OK) {
            throw PlayerDataError(sqlite_message(database_, status));
        }
    }

    void bind(sqlite3_int64 value, int index) {
        const int status = sqlite3_bind_int64(statement_, index, value);
        if (status != SQLITE_OK) {
            throw PlayerDataError(sqlite_message(database_, status));
        }
    }

    [[nodiscard]] int step() {
        const int status = sqlite3_step(statement_);
        if (status != SQLITE_ROW && status != SQLITE_DONE) {
            throw PlayerDataError(sqlite_message(database_, status));
        }
        return status;
    }

private:
    sqlite3* database_{nullptr};
    sqlite3_stmt* statement_{nullptr};
};

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

class SqlitePlayerDataStore::Impl final {
public:
    Impl(std::filesystem::path database_file, SqlitePlayerDataOptions options)
        : credential_hash_cost_(options.credential_hash_cost) {
        if (database_file.empty() ||
            options.busy_timeout <= std::chrono::milliseconds::zero() ||
            options.busy_timeout.count() > std::numeric_limits<int>::max()) {
            throw PlayerDataError("invalid player data store options");
        }
        if (sodium_init() < 0) {
            throw PlayerDataError("failed to initialize credential hashing");
        }
        if (database_file.has_parent_path()) {
            std::error_code error;
            std::filesystem::create_directories(
                database_file.parent_path(), error);
            if (error) {
                throw PlayerDataError(
                    "failed to create player data directory: " +
                    error.message());
            }
        }
        const int status = sqlite3_open_v2(
            database_file.string().c_str(),
            &database_,
            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
            nullptr);
        if (status != SQLITE_OK) {
            const auto message = sqlite_message(database_, status);
            sqlite3_close(database_);
            database_ = nullptr;
            throw PlayerDataError(message);
        }
        // 析构函数不会为构造中途抛出的对象运行,失败时在这里关闭句柄。
        try {
            sqlite3_extended_result_codes(database_, 1);
            if (sqlite3_busy_timeout(
                    database_,
                    static_cast<int>(options.busy_timeout.count())) !=
                SQLITE_OK) {
                throw PlayerDataError(
                    "failed to configure sqlite busy timeout");
            }
            migrate();
            if (options.bootstrap_accounts_file.has_value()) {
                static_cast<void>(
                    bootstrap_from_lua(*options.bootstrap_accounts_file));
            }
        } catch (...) {
            sqlite3_close(database_);
            database_ = nullptr;
            throw;
        }
    }

    ~Impl() { sqlite3_close(database_); }

    [[nodiscard]] std::optional<AccountRecord> authenticate(
        std::string_view account, std::string_view credential) const {
        AccountRecord record;
        std::string hash;
        {
            const std::scoped_lock lock(mutex_);
            Statement query(
                database_,
                "SELECT account_id, credential_hash, banned, whitelisted "
                "FROM accounts WHERE account_name = ?1");
            query.bind(account, 1);
            if (query.step() != SQLITE_ROW) return std::nullopt;
            const auto* hash_text = reinterpret_cast<const char*>(
                sqlite3_column_text(query.get(), 1));
            if (hash_text == nullptr) return std::nullopt;
            hash = hash_text;
            record = AccountRecord{
                .account_id = domain_id(sqlite3_column_int64(query.get(), 0)),
                .banned = sqlite3_column_int(query.get(), 2) != 0,
                .whitelisted = sqlite3_column_int(query.get(), 3) != 0,
            };
        }
        // Argon2 校验刻意耗时且占内存，不在连接锁内执行，避免把同一进程
        // 的其他查询（含 Realm/Gateway 读路径）串行化在口令校验之后。
        if (!credential_matches(hash, credential)) return std::nullopt;
        return record;
    }

    [[nodiscard]] std::optional<AccountLoginFacts> login_facts(
        std::uint64_t account_id) const {
        const std::scoped_lock lock(mutex_);
        Statement query(
            database_,
            "SELECT a.account_id, c.character_id, c.realm_id, c.revision "
            "FROM accounts AS a JOIN characters AS c "
            "ON c.character_id = a.selected_character_id "
            "AND c.account_id = a.account_id "
            "WHERE a.account_id = ?1 AND a.banned = 0 "
            "AND a.whitelisted = 1");
        query.bind(database_id(account_id, "account_id"), 1);
        if (query.step() != SQLITE_ROW) return std::nullopt;
        const auto realm = sqlite3_column_int64(query.get(), 2);
        if (realm <= 0 ||
            realm > static_cast<sqlite3_int64>(
                        std::numeric_limits<std::uint32_t>::max())) {
            throw PlayerDataError("database contains an invalid realm id");
        }
        return AccountLoginFacts{
            .account_id = domain_id(sqlite3_column_int64(query.get(), 0)),
            .character_id = domain_id(sqlite3_column_int64(query.get(), 1)),
            .realm_id = static_cast<std::uint32_t>(realm),
            .character_revision =
                domain_id(sqlite3_column_int64(query.get(), 3)),
        };
    }

    [[nodiscard]] std::optional<CharacterRecord> character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) const {
        const std::scoped_lock lock(mutex_);
        Statement query(
            database_,
            "SELECT character_id, account_id, realm_id, name, revision "
            "FROM characters WHERE character_id = ?1 AND account_id = ?2 "
            "AND realm_id = ?3");
        query.bind(database_id(character_id, "character_id"), 1);
        query.bind(database_id(account_id, "account_id"), 2);
        query.bind(static_cast<sqlite3_int64>(realm_id), 3);
        if (query.step() != SQLITE_ROW) return std::nullopt;
        const auto* name = reinterpret_cast<const char*>(
            sqlite3_column_text(query.get(), 3));
        return CharacterRecord{
            .character_id = domain_id(sqlite3_column_int64(query.get(), 0)),
            .account_id = domain_id(sqlite3_column_int64(query.get(), 1)),
            .realm_id = static_cast<std::uint32_t>(
                sqlite3_column_int64(query.get(), 2)),
            .name = name == nullptr ? std::string{} : std::string{name},
            .revision = domain_id(sqlite3_column_int64(query.get(), 4)),
        };
    }

    void provision_account(const AccountProvisioning& account) {
        if (account.account_name.empty()) {
            throw PlayerDataError("account name must not be empty");
        }
        const auto hash =
            credential_hash(account.credential, credential_hash_cost_);
        const std::scoped_lock lock(mutex_);
        Statement write(
            database_,
            "INSERT INTO accounts(account_id, account_name, credential_hash, "
            "banned, whitelisted) VALUES(?1, ?2, ?3, ?4, ?5) "
            "ON CONFLICT(account_id) DO UPDATE SET "
            "account_name = excluded.account_name, "
            "credential_hash = excluded.credential_hash, "
            "banned = excluded.banned, whitelisted = excluded.whitelisted");
        write.bind(database_id(account.account_id, "account_id"), 1);
        write.bind(account.account_name, 2);
        write.bind(hash, 3);
        write.bind(account.banned ? 1 : 0, 4);
        write.bind(account.whitelisted ? 1 : 0, 5);
        static_cast<void>(write.step());
    }

    void set_account_access(
        std::uint64_t account_id, bool banned, bool whitelisted) {
        const std::scoped_lock lock(mutex_);
        Statement write(
            database_,
            "UPDATE accounts SET banned = ?1, whitelisted = ?2 "
            "WHERE account_id = ?3");
        write.bind(banned ? 1 : 0, 1);
        write.bind(whitelisted ? 1 : 0, 2);
        write.bind(database_id(account_id, "account_id"), 3);
        static_cast<void>(write.step());
        if (sqlite3_changes(database_) != 1) {
            throw PlayerDataError("account does not exist");
        }
    }

    void provision_character(const CharacterRecord& character) {
        if (character.realm_id == 0 || character.name.empty() ||
            character.revision == 0) {
            throw PlayerDataError("character fields must be non-empty");
        }
        const std::scoped_lock lock(mutex_);
        Statement existing(
            database_,
            "SELECT account_id, realm_id FROM characters "
            "WHERE character_id = ?1");
        existing.bind(database_id(character.character_id, "character_id"), 1);
        if (existing.step() == SQLITE_ROW &&
            (domain_id(sqlite3_column_int64(existing.get(), 0)) !=
                 character.account_id ||
             static_cast<std::uint32_t>(
                 sqlite3_column_int64(existing.get(), 1)) !=
                 character.realm_id)) {
            throw PlayerDataError("character ownership cannot be reassigned");
        }
        Statement write(
            database_,
            "INSERT INTO characters(character_id, account_id, realm_id, name, "
            "revision) VALUES(?1, ?2, ?3, ?4, ?5) "
            "ON CONFLICT(character_id) DO UPDATE SET "
            "name = excluded.name, revision = excluded.revision");
        write.bind(database_id(character.character_id, "character_id"), 1);
        write.bind(database_id(character.account_id, "account_id"), 2);
        write.bind(static_cast<sqlite3_int64>(character.realm_id), 3);
        write.bind(character.name, 4);
        write.bind(database_id(character.revision, "revision"), 5);
        static_cast<void>(write.step());
    }

    void select_character(
        std::uint64_t account_id, std::uint64_t character_id) {
        const std::scoped_lock lock(mutex_);
        Statement write(
            database_,
            "UPDATE accounts SET selected_character_id = ?1 "
            "WHERE account_id = ?2 AND EXISTS("
            "SELECT 1 FROM characters WHERE character_id = ?1 "
            "AND account_id = ?2)");
        write.bind(database_id(character_id, "character_id"), 1);
        write.bind(database_id(account_id, "account_id"), 2);
        static_cast<void>(write.step());
        if (sqlite3_changes(database_) != 1) {
            throw PlayerDataError(
                "selected character must belong to the account");
        }
    }

    [[nodiscard]] bool bootstrap_from_lua(
        const std::filesystem::path& accounts_file) {
        // 每个服务启动都会进入这里;先做廉价检查,非空库不再读取与哈希 Lua 账号。
        {
            const std::scoped_lock lock(mutex_);
            if (!bootstrap_pending()) return false;
        }
        const auto accounts =
            load_bootstrap_accounts(accounts_file, credential_hash_cost_);
        const std::scoped_lock lock(mutex_);
        execute(database_, "BEGIN IMMEDIATE");
        try {
            // 哈希期间另一进程可能已完成导入,写锁内重新确认。
            if (!bootstrap_pending()) {
                execute(database_, "COMMIT");
                return false;
            }
            for (const auto& entry : accounts) {
                Statement account(
                    database_,
                    "INSERT INTO accounts(account_id, account_name, "
                    "credential_hash, banned, whitelisted, "
                    "selected_character_id) VALUES(?1, ?2, ?3, ?4, ?5, ?1)");
                account.bind(database_id(entry.account.account_id, "account_id"), 1);
                account.bind(entry.account.account_name, 2);
                account.bind(entry.hash, 3);
                account.bind(entry.account.banned ? 1 : 0, 4);
                account.bind(entry.account.whitelisted ? 1 : 0, 5);
                static_cast<void>(account.step());

                Statement character(
                    database_,
                    "INSERT INTO characters(character_id, account_id, realm_id, "
                    "name, revision) VALUES(?1, ?1, 1, ?2, 1)");
                character.bind(
                    database_id(entry.account.account_id, "account_id"), 1);
                character.bind(entry.account.account_name, 2);
                static_cast<void>(character.step());
            }
            execute(
                database_,
                "INSERT INTO store_metadata(key, value) VALUES("
                "'legacy_account_bootstrap', 'complete')");
            execute(database_, "COMMIT");
            return true;
        } catch (...) {
            try {
                execute(database_, "ROLLBACK");
            } catch (...) {
            }
            throw;
        }
    }

private:
    [[nodiscard]] bool bootstrap_pending() const {
        Statement bootstrapped(
            database_,
            "SELECT 1 FROM store_metadata "
            "WHERE key = 'legacy_account_bootstrap'");
        if (bootstrapped.step() == SQLITE_ROW) return false;
        Statement count(database_, "SELECT COUNT(*) FROM accounts");
        static_cast<void>(count.step());
        return sqlite3_column_int64(count.get(), 0) == 0;
    }

    void migrate() {
        execute(database_, "PRAGMA foreign_keys = ON");
        execute(database_, "PRAGMA journal_mode = WAL");
        execute(database_, "PRAGMA synchronous = FULL");
        execute(database_, "BEGIN IMMEDIATE");
        try {
            execute(
                database_,
                "CREATE TABLE IF NOT EXISTS schema_migrations("
                "version INTEGER PRIMARY KEY, applied_at TEXT NOT NULL "
                "DEFAULT CURRENT_TIMESTAMP)");
            Statement version(
                database_,
                "SELECT COALESCE(MAX(version), 0) FROM schema_migrations");
            static_cast<void>(version.step());
            const auto current = sqlite3_column_int64(version.get(), 0);
            if (current > 2) {
                throw PlayerDataError(
                    "player data schema is newer than this binary");
            }
            if (current == 0) {
                execute(
                    database_,
                    "CREATE TABLE accounts("
                    "account_id INTEGER PRIMARY KEY CHECK(account_id <> 0),"
                    "account_name TEXT NOT NULL UNIQUE,"
                    "credential_hash TEXT NOT NULL,"
                    "banned INTEGER NOT NULL DEFAULT 0 CHECK(banned IN (0,1)),"
                    "whitelisted INTEGER NOT NULL DEFAULT 0 "
                    "CHECK(whitelisted IN (0,1)),"
                    "selected_character_id INTEGER NULL)");
                execute(
                    database_,
                    "CREATE TABLE characters("
                    "character_id INTEGER PRIMARY KEY CHECK(character_id <> 0),"
                    "account_id INTEGER NOT NULL REFERENCES accounts(account_id) "
                    "ON DELETE CASCADE,"
                    "realm_id INTEGER NOT NULL CHECK(realm_id > 0),"
                    "name TEXT NOT NULL CHECK(length(name) > 0),"
                    "revision INTEGER NOT NULL DEFAULT 1 CHECK(revision > 0))");
                execute(
                    database_,
                    "CREATE INDEX characters_account_realm "
                    "ON characters(account_id, realm_id)");
                execute(
                    database_,
                    "INSERT INTO schema_migrations(version) VALUES(1)");
            }
            if (current <= 1) {
                execute(
                    database_,
                    "CREATE TABLE IF NOT EXISTS store_metadata("
                    "key TEXT PRIMARY KEY, value TEXT NOT NULL)");
                execute(
                    database_,
                    "INSERT OR IGNORE INTO schema_migrations(version) VALUES(2)");
            }
            execute(database_, "COMMIT");
        } catch (...) {
            try {
                execute(database_, "ROLLBACK");
            } catch (...) {
            }
            throw;
        }
    }

    CredentialHashCost credential_hash_cost_{CredentialHashCost::Interactive};
    sqlite3* database_{nullptr};
    mutable std::mutex mutex_;
};

SqlitePlayerDataStore::SqlitePlayerDataStore(
    std::filesystem::path database_file, SqlitePlayerDataOptions options)
    : impl_(std::make_unique<Impl>(std::move(database_file), options)) {}

SqlitePlayerDataStore::~SqlitePlayerDataStore() = default;

std::optional<AccountRecord> SqlitePlayerDataStore::authenticate(
    std::string_view account, std::string_view credential) const {
    return impl_->authenticate(account, credential);
}

std::optional<AccountLoginFacts> SqlitePlayerDataStore::login_facts(
    std::uint64_t account_id) const {
    return impl_->login_facts(account_id);
}

std::optional<CharacterRecord> SqlitePlayerDataStore::character(
    std::uint64_t account_id,
    std::uint32_t realm_id,
    std::uint64_t character_id) const {
    return impl_->character(account_id, realm_id, character_id);
}

void SqlitePlayerDataStore::provision_account(
    const AccountProvisioning& account) {
    impl_->provision_account(account);
}

void SqlitePlayerDataStore::set_account_access(
    std::uint64_t account_id, bool banned, bool whitelisted) {
    impl_->set_account_access(account_id, banned, whitelisted);
}

void SqlitePlayerDataStore::provision_character(
    const CharacterRecord& character) {
    impl_->provision_character(character);
}

void SqlitePlayerDataStore::select_character(
    std::uint64_t account_id, std::uint64_t character_id) {
    impl_->select_character(account_id, character_id);
}

bool SqlitePlayerDataStore::bootstrap_from_lua(
    const std::filesystem::path& accounts_file) {
    return impl_->bootstrap_from_lua(accounts_file);
}

}  // namespace realm::game::common
