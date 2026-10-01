#include "realmmesh/game/common/player_data_store.hpp"

#include "realmmesh/test_support/mongod_process.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <fstream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace realm::game::common {
namespace {

using namespace std::chrono_literals;

class PlayerDataStoreTest : public ::testing::Test {
protected:
    [[nodiscard]] MongoPlayerDataStore open_store(
        MongoPlayerDataOptions options = {}) const {
        return MongoPlayerDataStore(mongod_.uri(), database_, std::move(options));
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
        return mongod_.eval(
            database_,
            "print(db.accounts.findOne({account_name: '" +
                std::string(account) + "'}).credential_hash)");
    }

    test_support::MongodProcess& mongod_{test_support::MongodProcess::shared()};
    std::string database_{test_support::MongodProcess::fresh_database()};
    test_support::TemporaryDirectory directory_{"player-data-store-test-"};
};

TEST_F(PlayerDataStoreTest, PersistsAccountAndSelectedCharacterAcrossReconnect) {
    {
        auto store = open_store();
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

    auto reopened = open_store();
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

TEST_F(PlayerDataStoreTest, CommittedAccessUpdatesAreVisibleToOtherStores) {
    auto writer = open_store();
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

    auto reader = open_store();
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
    auto store = open_store();
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
    auto store = open_store();
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

    auto store = open_store();
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

    // Gateway/Realm 可能先于 Login Verifier 连上空库；任何一方打开都应
    // 原子完成同一次导入，而不是读到一个永远没有账号的库。
    const auto reader = open_store(
        MongoPlayerDataOptions{.bootstrap_accounts_file = accounts});
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
    const auto later = open_store(
        MongoPlayerDataOptions{.bootstrap_accounts_file = replacement});
    EXPECT_TRUE(later.authenticate("player", "secret").has_value());
    EXPECT_FALSE(later.login_facts(99).has_value());
}

TEST_F(PlayerDataStoreTest, PopulatedDatabaseSkipsBootstrapWithoutReadingTheFile) {
    auto store = open_store();
    store.provision_account(AccountProvisioning{
        .account_id = 42,
        .account_name = "player",
        .credential = "secret",
        .whitelisted = true,
    });

    // 每个服务启动都会走导入入口；库非空时不应再读取并逐个哈希 Lua 账号。
    const auto unreadable = write_accounts("this is not lua");
    EXPECT_FALSE(store.bootstrap_from_lua(unreadable));
    EXPECT_NO_THROW(static_cast<void>(open_store(
        MongoPlayerDataOptions{.bootstrap_accounts_file = unreadable})));
}

TEST_F(PlayerDataStoreTest, CredentialHashCostIsRecordedInTheStoredHash) {
    {
        auto store = open_store(MongoPlayerDataOptions{
            .credential_hash_cost = CredentialHashCost::Minimum});
        store.provision_account(AccountProvisioning{
            .account_id = 42,
            .account_name = "robot",
            .credential = "secret",
            .whitelisted = true,
        });
    }
    {
        auto store = open_store();
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

TEST_F(PlayerDataStoreTest, RejectsReassigningCharacterOwnership) {
    auto store = open_store();
    for (const std::uint64_t account : {42U, 43U}) {
        store.provision_account(AccountProvisioning{
            .account_id = account,
            .account_name = "player-" + std::to_string(account),
            .credential = "secret",
            .whitelisted = true,
        });
    }
    store.provision_character(CharacterRecord{
        .character_id = 7001,
        .account_id = 42,
        .realm_id = 1,
        .name = "Ranger",
    });

    EXPECT_THROW(
        store.provision_character(CharacterRecord{
            .character_id = 7001,
            .account_id = 43,
            .realm_id = 1,
            .name = "Thief",
        }),
        PlayerDataError);
    EXPECT_THROW(
        store.provision_character(CharacterRecord{
            .character_id = 7001,
            .account_id = 42,
            .realm_id = 2,
            .name = "Ranger",
        }),
        PlayerDataError);
    // 同一归属下重新开通只更新名字与版本。
    store.provision_character(CharacterRecord{
        .character_id = 7001,
        .account_id = 42,
        .realm_id = 1,
        .name = "Renamed",
        .revision = 4,
    });
    const auto character = store.character(42, 1, 7001);
    ASSERT_TRUE(character.has_value());
    EXPECT_EQ(character->name, "Renamed");
    EXPECT_EQ(character->revision, 4U);
}

TEST_F(PlayerDataStoreTest, RejectsAccountNameTakenByAnotherId) {
    auto store = open_store();
    store.provision_account(AccountProvisioning{
        .account_id = 42,
        .account_name = "player",
        .credential = "secret",
    });
    EXPECT_THROW(
        store.provision_account(AccountProvisioning{
            .account_id = 43,
            .account_name = "player",
            .credential = "secret",
        }),
        PlayerDataError);
}

TEST_F(PlayerDataStoreTest, ReprovisioningAccountKeepsSelectedCharacter) {
    auto store = open_store();
    const AccountProvisioning account{
        .account_id = 42,
        .account_name = "player",
        .credential = "secret",
        .whitelisted = true,
    };
    store.provision_account(account);
    store.provision_character(CharacterRecord{
        .character_id = 7001,
        .account_id = 42,
        .realm_id = 1,
        .name = "Ranger",
    });
    store.select_character(42, 7001);

    auto rotated = account;
    rotated.credential = "rotated";
    store.provision_account(rotated);
    EXPECT_FALSE(store.authenticate("player", "secret").has_value());
    EXPECT_TRUE(store.authenticate("player", "rotated").has_value());
    const auto facts = store.login_facts(42);
    ASSERT_TRUE(facts.has_value());
    EXPECT_EQ(facts->character_id, 7001U);
}

TEST_F(PlayerDataStoreTest, ConcurrentFirstStartsImportTheBootstrapOnce) {
    const auto accounts = write_accounts(R"lua(
return {
    accounts = {
        { account = "player", credential = "secret", account_id = 42,
          whitelisted = true },
        { account = "other", credential = "secret", account_id = 43,
          whitelisted = true },
    },
}
)lua");
    // 三个服务共用同一配置并发启动：只有一方完成导入，其余看到已导入的
    // 库而正常启动。
    std::vector<std::optional<MongoPlayerDataStore>> stores(3);
    std::vector<std::exception_ptr> errors(stores.size());
    {
        std::vector<std::jthread> starters;
        for (std::size_t index = 0; index < stores.size(); ++index) {
            starters.emplace_back([&, index] {
                try {
                    stores[index].emplace(
                        mongod_.uri(),
                        database_,
                        MongoPlayerDataOptions{
                            .bootstrap_accounts_file = accounts,
                            .credential_hash_cost =
                                CredentialHashCost::Minimum});
                } catch (...) {
                    errors[index] = std::current_exception();
                }
            });
        }
    }
    for (const auto& error : errors) {
        if (error) std::rethrow_exception(error);
    }
    EXPECT_EQ(mongod_.eval(database_, "print(db.accounts.countDocuments())"),
              "2");
    EXPECT_EQ(
        mongod_.eval(database_, "print(db.characters.countDocuments())"), "2");
    EXPECT_TRUE(stores.front()->authenticate("other", "secret").has_value());
}

TEST_F(PlayerDataStoreTest, RefusesSchemaNewerThanThisBinary) {
    static_cast<void>(open_store());
    static_cast<void>(mongod_.eval(
        database_,
        "db.store_metadata.updateOne({_id: 'schema'}, "
        "{$set: {version: NumberLong(2)}})"));
    EXPECT_THROW(static_cast<void>(open_store()), PlayerDataError);
}

TEST_F(PlayerDataStoreTest, UnreachableDeploymentFailsWithinServerSelectionTimeout) {
    const auto port = test_support::unused_loopback_ports(1).at(0);
    const auto started = std::chrono::steady_clock::now();
    EXPECT_THROW(
        static_cast<void>(MongoPlayerDataStore(
            "mongodb://127.0.0.1:" + std::to_string(port) + "/?replicaSet=rs0",
            database_,
            MongoPlayerDataOptions{.server_selection_timeout = 200ms})),
        PlayerDataError);
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
}

TEST_F(PlayerDataStoreTest, RejectsTimeoutsEmbeddedInTheUri) {
    // 超时只能来自 player_data 配置，URI 自带同名参数直接拒绝。
    EXPECT_THROW(
        static_cast<void>(MongoPlayerDataStore(
            mongod_.uri() + "&socketTimeoutMS=10", database_)),
        PlayerDataError);
    EXPECT_THROW(
        static_cast<void>(MongoPlayerDataStore(mongod_.uri(), "")),
        PlayerDataError);
}

}  // namespace
}  // namespace realm::game::common
