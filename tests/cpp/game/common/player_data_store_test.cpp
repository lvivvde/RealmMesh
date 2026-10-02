#include "realmmesh/game/common/player_data_store.hpp"

#include "realmmesh/test_support/mongod_process.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <algorithm>
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

    [[nodiscard]] std::string stored_character_field(
        std::uint64_t character_id, std::string_view field) const {
        return mongod_.eval(
            database_,
            "print(db.characters.findOne({_id: NumberLong('" +
                std::to_string(static_cast<std::int64_t>(character_id)) +
                "')})." + std::string(field) + ".toString())");
    }

    void provision_player(std::uint64_t account_id, std::string name) {
        auto store = open_store();
        store.provision_account(AccountProvisioning{
            .account_id = account_id,
            .account_name = std::move(name),
            .credential = "secret",
            .whitelisted = true,
        });
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

    const auto roster = reopened.list_characters(42, 1);
    ASSERT_EQ(roster.characters.size(), 1U);
    EXPECT_EQ(roster.characters[0].name, "Ranger");
    EXPECT_EQ(roster.last_selected_character_id, 7001U);
    EXPECT_EQ(stored_character_field(7001, "revision"), "3");
    EXPECT_FALSE(reopened.realm_character(43, 1, 7001).has_value());
    EXPECT_FALSE(reopened.realm_character(42, 2, 7001).has_value());
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
    EXPECT_EQ(facts->account_id, account_id);
    EXPECT_EQ(
        store.list_characters(account_id, 1).last_selected_character_id,
        character_id);
    EXPECT_TRUE(store.realm_character(account_id, 1, character_id).has_value());
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
    EXPECT_EQ(store.list_characters(42, 1).last_selected_character_id, 0U);
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
    ASSERT_TRUE(store.login_facts(42).has_value());
    // Lua 首次导入为每个开发账号建一个同号默认角色,并设为上次所选。
    const auto roster = store.list_characters(42, 1);
    ASSERT_EQ(roster.characters.size(), 1U);
    EXPECT_EQ(roster.characters[0].character_id, 42U);
    EXPECT_EQ(roster.characters[0].name, "player");
    EXPECT_EQ(roster.characters[0].exp, 0U);
    EXPECT_EQ(roster.characters[0].last_training_seq, 0U);
    EXPECT_EQ(roster.last_selected_character_id, 42U);

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
    ASSERT_TRUE(reader.login_facts(42).has_value());
    EXPECT_EQ(reader.list_characters(42, 1).last_selected_character_id, 42U);

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
    const auto character = store.realm_character(42, 1, 7001);
    ASSERT_TRUE(character.has_value());
    EXPECT_EQ(character->name, "Renamed");
    EXPECT_EQ(stored_character_field(7001, "revision"), "4");
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
    EXPECT_EQ(store.list_characters(42, 1).last_selected_character_id, 7001U);
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

/// ADR-0013:准入只看封禁与白名单,没有角色的新账号同样可以入场。
TEST_F(PlayerDataStoreTest, AccountWithoutCharactersIsEligible) {
    provision_player(42, "fresh");
    auto store = open_store();
    ASSERT_TRUE(store.login_facts(42).has_value());
    const auto roster = store.list_characters(42, 1);
    EXPECT_TRUE(roster.characters.empty());
    EXPECT_EQ(roster.last_selected_character_id, 0U);
}

TEST_F(PlayerDataStoreTest, CreatedCharacterStartsAtZeroAndIsListedInOrder) {
    provision_player(42, "player");
    auto store = open_store();
    const auto first = store.create_character(42, 1, "Alpha", 3);
    const auto second = store.create_character(42, 1, "Beta", 3);
    ASSERT_EQ(first.outcome, CreateCharacterOutcome::Created);
    ASSERT_EQ(second.outcome, CreateCharacterOutcome::Created);
    EXPECT_NE(first.character.character_id, 0U);
    EXPECT_LT(first.character.character_id, 1ULL << 63U);
    EXPECT_NE(first.character.character_id, second.character.character_id);
    EXPECT_EQ(first.character.name, "Alpha");
    EXPECT_EQ(first.character.exp, 0U);
    EXPECT_EQ(first.character.last_training_seq, 0U);

    const auto roster = store.list_characters(42, 1);
    ASSERT_EQ(roster.characters.size(), 2U);
    EXPECT_EQ(roster.characters[0].character_id, first.character.character_id);
    EXPECT_EQ(roster.characters[1].character_id, second.character.character_id);
    EXPECT_TRUE(store.list_characters(42, 2).characters.empty());
}

TEST_F(PlayerDataStoreTest, CharacterNamesAreUniquePerRealm) {
    provision_player(42, "player");
    provision_player(43, "other");
    auto store = open_store();
    ASSERT_EQ(
        store.create_character(42, 1, "Ranger", 3).outcome,
        CreateCharacterOutcome::Created);
    EXPECT_EQ(
        store.create_character(43, 1, "Ranger", 3).outcome,
        CreateCharacterOutcome::NameTaken);
    // 精确匹配:大小写不同即为不同名字;别的 Realm 可以同名。
    EXPECT_EQ(
        store.create_character(43, 1, "ranger", 3).outcome,
        CreateCharacterOutcome::Created);
    EXPECT_EQ(
        store.create_character(43, 2, "Ranger", 3).outcome,
        CreateCharacterOutcome::Created);
}

TEST_F(PlayerDataStoreTest, CharacterCountIsLimitedPerAccountAndRealm) {
    provision_player(42, "player");
    auto store = open_store();
    for (const auto* name : {"A", "B", "C"}) {
        ASSERT_EQ(
            store.create_character(42, 1, name, 3).outcome,
            CreateCharacterOutcome::Created);
    }
    EXPECT_EQ(
        store.create_character(42, 1, "D", 3).outcome,
        CreateCharacterOutcome::LimitReached);
    EXPECT_EQ(
        store.create_character(42, 2, "D", 3).outcome,
        CreateCharacterOutcome::Created);
    EXPECT_EQ(store.list_characters(42, 1).characters.size(), 3U);
}

/// 上限在存储内原子判定:并发创建(如被顶号的旧会话仍在途)不会越界。
TEST_F(PlayerDataStoreTest, ConcurrentCreatesNeverExceedTheLimit) {
    provision_player(42, "player");
    auto store = open_store();
    constexpr int attempts = 6;
    std::vector<CreateCharacterOutcome> outcomes(attempts);
    {
        std::vector<std::jthread> creators;
        for (int index = 0; index < attempts; ++index) {
            creators.emplace_back([&, index] {
                outcomes[static_cast<std::size_t>(index)] =
                    store
                        .create_character(
                            42, 1, "Hero" + std::to_string(index), 3)
                        .outcome;
            });
        }
    }
    EXPECT_EQ(
        std::ranges::count(outcomes, CreateCharacterOutcome::Created), 3);
    EXPECT_EQ(
        std::ranges::count(outcomes, CreateCharacterOutcome::LimitReached), 3);
    EXPECT_EQ(store.list_characters(42, 1).characters.size(), 3U);
}

/// 两个账号同时抢同一个名字:只有一人成功,另一人得到 NameTaken。
TEST_F(PlayerDataStoreTest, ConcurrentSameNameCreatesHaveOneWinner) {
    constexpr int attempts = 4;
    for (int index = 0; index < attempts; ++index) {
        provision_player(
            static_cast<std::uint64_t>(42 + index),
            "player" + std::to_string(index));
    }
    auto store = open_store();
    std::vector<CreateCharacterOutcome> outcomes(attempts);
    {
        std::vector<std::jthread> creators;
        for (int index = 0; index < attempts; ++index) {
            creators.emplace_back([&, index] {
                outcomes[static_cast<std::size_t>(index)] =
                    store
                        .create_character(
                            static_cast<std::uint64_t>(42 + index), 1, "Hero", 3)
                        .outcome;
            });
        }
    }
    EXPECT_EQ(
        std::ranges::count(outcomes, CreateCharacterOutcome::Created), 1);
    EXPECT_EQ(
        std::ranges::count(outcomes, CreateCharacterOutcome::NameTaken),
        attempts - 1);
}

TEST_F(PlayerDataStoreTest, ChoosingVerifiesOwnershipAndRecordsLastSelected) {
    provision_player(42, "player");
    provision_player(43, "other");
    auto store = open_store();
    const auto mine = store.create_character(42, 1, "Mine", 3).character;
    const auto theirs = store.create_character(43, 1, "Theirs", 3).character;
    const auto elsewhere = store.create_character(42, 2, "Elsewhere", 3).character;

    EXPECT_FALSE(store.choose_character(42, 1, theirs.character_id).has_value());
    EXPECT_FALSE(
        store.choose_character(42, 1, elsewhere.character_id).has_value());
    EXPECT_EQ(store.list_characters(42, 1).last_selected_character_id, 0U);

    const auto chosen = store.choose_character(42, 1, mine.character_id);
    ASSERT_TRUE(chosen.has_value());
    EXPECT_EQ(chosen->name, "Mine");
    EXPECT_EQ(
        store.list_characters(42, 1).last_selected_character_id,
        mine.character_id);
    // 上次所选属于别的 Realm 时,本 Realm 的列表不给出它。
    ASSERT_TRUE(store.choose_character(42, 2, elsewhere.character_id));
    EXPECT_EQ(store.list_characters(42, 1).last_selected_character_id, 0U);
}

TEST_F(PlayerDataStoreTest, TrainingWriteIsConditionalOnThePreviousSeq) {
    provision_player(42, "player");
    auto store = open_store();
    const auto id = store.create_character(42, 1, "Trainee", 3)
                        .character.character_id;

    const auto first = store.record_training(TrainingWrite{
        .account_id = 42, .realm_id = 1, .character_id = id, .seq = 1, .exp = 10});
    ASSERT_TRUE(first.has_value());
    EXPECT_TRUE(first->committed);
    EXPECT_EQ(first->character.exp, 10U);
    EXPECT_EQ(first->character.last_training_seq, 1U);
    EXPECT_EQ(stored_character_field(id, "revision"), "2");

    // 同一 seq 再写一次:条件不满足,不改状态,返回当前已确认状态。
    const auto again = store.record_training(TrainingWrite{
        .account_id = 42, .realm_id = 1, .character_id = id, .seq = 1, .exp = 20});
    ASSERT_TRUE(again.has_value());
    EXPECT_FALSE(again->committed);
    EXPECT_EQ(again->character.exp, 10U);
    EXPECT_EQ(again->character.last_training_seq, 1U);

    const auto skipped = store.record_training(TrainingWrite{
        .account_id = 42, .realm_id = 1, .character_id = id, .seq = 3, .exp = 20});
    ASSERT_TRUE(skipped.has_value());
    EXPECT_FALSE(skipped->committed);
    EXPECT_EQ(stored_character_field(id, "revision"), "2");

    EXPECT_FALSE(store
                     .record_training(TrainingWrite{
                         .account_id = 43,
                         .realm_id = 1,
                         .character_id = id,
                         .seq = 2,
                         .exp = 20})
                     .has_value());
}

/// schema 1 的角色没有经验与训练序号;打开 v2 存储时补 0 并升级标记。
TEST_F(PlayerDataStoreTest, MigratesSchemaOneCharacters) {
    static_cast<void>(mongod_.eval(
        database_,
        "db.store_metadata.insertOne({_id: 'schema', version: NumberLong(1)});"
        "db.accounts.insertOne({_id: NumberLong(42), account_name: 'old',"
        " credential_hash: 'x', banned: false, whitelisted: true,"
        " selected_character_id: NumberLong(7001)});"
        "db.characters.insertOne({_id: NumberLong(7001),"
        " account_id: NumberLong(42), realm_id: NumberLong(1),"
        " name: 'Veteran', revision: NumberLong(5)})"));

    auto store = open_store();
    const auto veteran = store.realm_character(42, 1, 7001);
    ASSERT_TRUE(veteran.has_value());
    EXPECT_EQ(veteran->exp, 0U);
    EXPECT_EQ(veteran->last_training_seq, 0U);
    EXPECT_EQ(
        mongod_.eval(
            database_,
            "print(db.store_metadata.findOne({_id: 'schema'}).version"
            ".toString())"),
        "2");
    EXPECT_TRUE(store
                    .record_training(TrainingWrite{
                        .account_id = 42,
                        .realm_id = 1,
                        .character_id = 7001,
                        .seq = 1,
                        .exp = 10})
                    ->committed);
}

TEST_F(PlayerDataStoreTest, RefusesSchemaNewerThanThisBinary) {
    static_cast<void>(open_store());
    static_cast<void>(mongod_.eval(
        database_,
        "db.store_metadata.updateOne({_id: 'schema'}, "
        "{$set: {version: NumberLong(3)}})"));
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
