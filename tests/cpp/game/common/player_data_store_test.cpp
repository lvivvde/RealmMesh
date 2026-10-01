#include "realmmesh/game/common/player_data_store.hpp"

#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <fstream>
#include <string>

namespace realm::game::common {
namespace {

class PlayerDataStoreTest : public ::testing::Test {
protected:
    [[nodiscard]] std::filesystem::path database_path() const {
        return directory_.path() / "player-data.sqlite";
    }

    [[nodiscard]] std::filesystem::path write_accounts(
        std::string_view contents) const {
        const auto path = directory_.path() / "accounts.lua";
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << contents;
        return path;
    }

    [[nodiscard]] std::string stored_credential_hash(
        std::string_view account) const {
        sqlite3* database = nullptr;
        EXPECT_EQ(
            sqlite3_open_v2(
                database_path().string().c_str(),
                &database,
                SQLITE_OPEN_READONLY,
                nullptr),
            SQLITE_OK);
        sqlite3_stmt* query = nullptr;
        EXPECT_EQ(
            sqlite3_prepare_v2(
                database,
                "SELECT credential_hash FROM accounts WHERE account_name = ?1",
                -1,
                &query,
                nullptr),
            SQLITE_OK);
        sqlite3_bind_text(
            query, 1, account.data(), static_cast<int>(account.size()),
            SQLITE_TRANSIENT);
        std::string hash;
        if (sqlite3_step(query) == SQLITE_ROW) {
            hash = reinterpret_cast<const char*>(sqlite3_column_text(query, 0));
        }
        sqlite3_finalize(query);
        sqlite3_close(database);
        return hash;
    }

    test_support::TemporaryDirectory directory_{"player-data-store-test-"};
};

TEST_F(PlayerDataStoreTest, PersistsAccountAndSelectedCharacterAcrossRestart) {
    {
        SqlitePlayerDataStore store(database_path());
        store.provision_account(AccountProvisioning{
            .account_id = 42,
            .account_name = "player",
            .credential = "secret",
            .whitelisted = true,
        });
        store.provision_character(CharacterRecord{
            .character_id = 7001,
            .account_id = 42,
            .realm_id = 1,
            .name = "Ranger",
            .revision = 3,
        });
        store.select_character(42, 7001);
    }

    SqlitePlayerDataStore reopened(database_path());
    const auto account = reopened.authenticate("player", "secret");
    ASSERT_TRUE(account.has_value());
    EXPECT_EQ(account->account_id, 42U);
    EXPECT_FALSE(account->banned);
    EXPECT_TRUE(account->whitelisted);

    const auto facts = reopened.login_facts(42);
    ASSERT_TRUE(facts.has_value());
    EXPECT_EQ(facts->account_id, 42U);
    EXPECT_EQ(facts->character_id, 7001U);
    EXPECT_EQ(facts->realm_id, 1U);
    EXPECT_EQ(facts->character_revision, 3U);

    const auto character = reopened.character(42, 1, 7001);
    ASSERT_TRUE(character.has_value());
    EXPECT_EQ(character->name, "Ranger");
    EXPECT_EQ(character->revision, 3U);
    EXPECT_FALSE(reopened.character(43, 1, 7001).has_value());
    EXPECT_FALSE(reopened.character(42, 2, 7001).has_value());
}

TEST_F(PlayerDataStoreTest, CommittedAccessUpdatesAreVisibleAcrossConnections) {
    SqlitePlayerDataStore writer(database_path());
    writer.provision_account(AccountProvisioning{
        .account_id = 42,
        .account_name = "player",
        .credential = "secret",
        .whitelisted = true,
    });
    writer.provision_character(CharacterRecord{
        .character_id = 7001,
        .account_id = 42,
        .realm_id = 1,
        .name = "Ranger",
    });
    writer.select_character(42, 7001);

    SqlitePlayerDataStore reader(database_path());
    ASSERT_TRUE(reader.login_facts(42).has_value());

    writer.set_account_access(42, true, true);
    const auto banned = reader.authenticate("player", "secret");
    ASSERT_TRUE(banned.has_value());
    EXPECT_TRUE(banned->banned);
    EXPECT_FALSE(reader.login_facts(42).has_value());

    writer.set_account_access(42, false, false);
    const auto outside = reader.authenticate("player", "secret");
    ASSERT_TRUE(outside.has_value());
    EXPECT_FALSE(outside->whitelisted);
    EXPECT_FALSE(reader.login_facts(42).has_value());
}

TEST_F(PlayerDataStoreTest, PreservesFullUnsignedDomainIds) {
    constexpr std::uint64_t account_id = 0xF000'0000'0000'0001ULL;
    constexpr std::uint64_t character_id = 0xE000'0000'0000'0002ULL;
    SqlitePlayerDataStore store(database_path());
    store.provision_account(AccountProvisioning{
        .account_id = account_id,
        .account_name = "wide-id",
        .credential = "secret",
        .whitelisted = true,
    });
    store.provision_character(CharacterRecord{
        .character_id = character_id,
        .account_id = account_id,
        .realm_id = 1,
        .name = "Wide",
    });
    store.select_character(account_id, character_id);

    const auto authenticated = store.authenticate("wide-id", "secret");
    ASSERT_TRUE(authenticated.has_value());
    EXPECT_EQ(authenticated->account_id, account_id);
    const auto facts = store.login_facts(account_id);
    ASSERT_TRUE(facts.has_value());
    EXPECT_EQ(facts->character_id, character_id);
    EXPECT_TRUE(store.character(account_id, 1, character_id).has_value());
}

TEST_F(PlayerDataStoreTest, RejectsSelectingCharacterOwnedByAnotherAccount) {
    SqlitePlayerDataStore store(database_path());
    store.provision_account(AccountProvisioning{
        .account_id = 42,
        .account_name = "player",
        .credential = "secret",
        .whitelisted = true,
    });
    store.provision_account(AccountProvisioning{
        .account_id = 43,
        .account_name = "other",
        .credential = "secret",
        .whitelisted = true,
    });
    store.provision_character(CharacterRecord{
        .character_id = 7001,
        .account_id = 43,
        .realm_id = 1,
        .name = "Other",
    });

    EXPECT_THROW(store.select_character(42, 7001), PlayerDataError);
    EXPECT_FALSE(store.login_facts(42).has_value());
}

TEST_F(PlayerDataStoreTest, BootstrapsLegacyConfigOnlyWhenDatabaseIsEmpty) {
    const auto accounts = directory_.path() / "accounts.lua";
    {
        std::ofstream stream(accounts, std::ios::binary);
        stream << R"lua(
return {
    accounts = {
        { account = "player", credential = "secret", account_id = 42,
          whitelisted = true },
    },
}
)lua";
    }

    SqlitePlayerDataStore store(database_path());
    EXPECT_TRUE(store.bootstrap_from_lua(accounts));
    ASSERT_TRUE(store.authenticate("player", "secret").has_value());
    const auto facts = store.login_facts(42);
    ASSERT_TRUE(facts.has_value());
    EXPECT_EQ(facts->character_id, 42U);
    EXPECT_EQ(facts->realm_id, 1U);
    const auto character = store.character(42, 1, 42);
    ASSERT_TRUE(character.has_value());
    EXPECT_EQ(character->name, "player");

    {
        std::ofstream stream(accounts, std::ios::binary | std::ios::trunc);
        stream << R"lua(
return {
    accounts = {
        { account = "replacement", credential = "changed", account_id = 99,
          whitelisted = true },
    },
}
)lua";
    }
    EXPECT_FALSE(store.bootstrap_from_lua(accounts));
    EXPECT_TRUE(store.authenticate("player", "secret").has_value());
    EXPECT_FALSE(store.authenticate("replacement", "changed").has_value());
}

TEST_F(PlayerDataStoreTest, AnyProcessOpeningAnEmptyDatabaseRunsTheBootstrap) {
    const auto accounts = write_accounts(R"lua(
return {
    accounts = {
        { account = "player", credential = "secret", account_id = 42,
          whitelisted = true },
    },
}
)lua");

    // Gateway/Realm 可能先于 Login Verifier 打开空库；任何一方打开都应
    // 原子完成同一次导入，而不是读到一个永远没有账号的库。
    const SqlitePlayerDataStore reader(
        database_path(),
        SqlitePlayerDataOptions{.bootstrap_accounts_file = accounts});
    const auto facts = reader.login_facts(42);
    ASSERT_TRUE(facts.has_value());
    EXPECT_EQ(facts->character_id, 42U);

    const auto replacement = write_accounts(R"lua(
return {
    accounts = {
        { account = "replacement", credential = "changed", account_id = 99,
          whitelisted = true },
    },
}
)lua");
    const SqlitePlayerDataStore later(
        database_path(),
        SqlitePlayerDataOptions{.bootstrap_accounts_file = replacement});
    EXPECT_TRUE(later.authenticate("player", "secret").has_value());
    EXPECT_FALSE(later.login_facts(99).has_value());
}

TEST_F(PlayerDataStoreTest, PopulatedDatabaseSkipsBootstrapWithoutReadingTheFile) {
    SqlitePlayerDataStore store(database_path());
    store.provision_account(AccountProvisioning{
        .account_id = 42,
        .account_name = "player",
        .credential = "secret",
        .whitelisted = true,
    });

    // 每个服务启动都会走导入入口；库非空时不应再读取并逐个哈希 Lua 账号。
    const auto unreadable = write_accounts("this is not lua");
    EXPECT_FALSE(store.bootstrap_from_lua(unreadable));
    EXPECT_NO_THROW(static_cast<void>(SqlitePlayerDataStore(
        database_path(),
        SqlitePlayerDataOptions{.bootstrap_accounts_file = unreadable})));
}

TEST_F(PlayerDataStoreTest, CredentialHashCostIsRecordedInTheStoredHash) {
    {
        SqlitePlayerDataStore store(
            database_path(),
            SqlitePlayerDataOptions{
                .credential_hash_cost = CredentialHashCost::Minimum});
        store.provision_account(AccountProvisioning{
            .account_id = 42,
            .account_name = "robot",
            .credential = "secret",
            .whitelisted = true,
        });
    }
    {
        SqlitePlayerDataStore store(database_path());
        store.provision_account(AccountProvisioning{
            .account_id = 43,
            .account_name = "player",
            .credential = "secret",
            .whitelisted = true,
        });
        // 校验参数取自哈希串本身：换成默认成本的进程仍能认证旧账号。
        EXPECT_TRUE(store.authenticate("robot", "secret").has_value());
        EXPECT_FALSE(store.authenticate("robot", "wrong").has_value());
    }

    EXPECT_NE(stored_credential_hash("robot").find("m=8,t=1,"),
              std::string::npos);
    EXPECT_NE(stored_credential_hash("player").find("m=65536,t=2,"),
              std::string::npos);
}

}  // namespace
}  // namespace realm::game::common
