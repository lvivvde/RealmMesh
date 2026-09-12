#include "realmmesh/game/common/account_store.hpp"

#include "realmmesh/test_support/temporary_directory.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string_view>

#include <gtest/gtest.h>

namespace realm::game::common {
namespace {

// 只测 AccountStore 接口外显行为:命中/未命中的查询结果与装载失败报错。
class AccountStoreTest : public ::testing::Test {
protected:
    [[nodiscard]] std::filesystem::path write_accounts(
        std::string_view lua) const {
        const auto path = directory_.path() / "accounts.lua";
        std::ofstream stream(path, std::ios::binary);
        stream << lua;
        return path;
    }

    test_support::TemporaryDirectory directory_{"account-store-test-"};
};

constexpr std::string_view kSampleAccounts = R"lua(
return {
    accounts = {
        { account = "player",   credential = "dev", whitelisted = true },
        { account = "pinned",   credential = "dev", whitelisted = true, account_id = 4242 },
        { account = "banned",   credential = "dev", whitelisted = true, banned = true },
        { account = "outsider", credential = "dev" },
    },
}
)lua";

// 派生规则钉死摘要值:FNV-1a("player"),零值回绕为 1。测试不镜像实现
// 的算法,实现若改派生规则这里即红。

TEST_F(AccountStoreTest, AuthenticatesValidAccountWithDerivedId) {
    const auto store =
        ConfigAccountStore::load(write_accounts(kSampleAccounts));
    const auto record = store.authenticate("player", "dev");
    ASSERT_TRUE(record.has_value());
    EXPECT_FALSE(record->banned);
    EXPECT_TRUE(record->whitelisted);
    EXPECT_EQ(record->account_id, 5008278420455340480ULL);
}

TEST_F(AccountStoreTest, HonorsPinnedAccountId) {
    const auto store =
        ConfigAccountStore::load(write_accounts(kSampleAccounts));
    const auto record = store.authenticate("pinned", "dev");
    ASSERT_TRUE(record.has_value());
    EXPECT_EQ(record->account_id, 4242ULL);
}

TEST_F(AccountStoreTest, WrongCredentialReturnsNothing) {
    const auto store =
        ConfigAccountStore::load(write_accounts(kSampleAccounts));
    EXPECT_FALSE(store.authenticate("player", "wrong").has_value());
}

TEST_F(AccountStoreTest, UnknownAccountReturnsNothing) {
    const auto store =
        ConfigAccountStore::load(write_accounts(kSampleAccounts));
    EXPECT_FALSE(store.authenticate("ghost", "dev").has_value());
}

TEST_F(AccountStoreTest, ExposesBannedFlag) {
    const auto store =
        ConfigAccountStore::load(write_accounts(kSampleAccounts));
    const auto record = store.authenticate("banned", "dev");
    ASSERT_TRUE(record.has_value());
    EXPECT_TRUE(record->banned);
    EXPECT_TRUE(record->whitelisted);
}

TEST_F(AccountStoreTest, ExposesWhitelistFlags) {
    const auto store =
        ConfigAccountStore::load(write_accounts(kSampleAccounts));
    const auto outsider = store.authenticate("outsider", "dev");
    ASSERT_TRUE(outsider.has_value());
    EXPECT_FALSE(outsider->banned);
    EXPECT_FALSE(outsider->whitelisted);
}

TEST_F(AccountStoreTest, LoadsEmptyAccountsTable) {
    constexpr std::string_view empty = R"lua(return { accounts = {} })lua";
    const auto store = ConfigAccountStore::load(write_accounts(empty));
    EXPECT_FALSE(store.authenticate("player", "dev").has_value());
}

TEST_F(AccountStoreTest, RejectsDuplicateAccountName) {
    constexpr std::string_view duplicate = R"lua(
return {
    accounts = {
        { account = "player", credential = "dev" },
        { account = "player", credential = "dev2" },
    },
}
)lua";
    EXPECT_THROW(
        static_cast<void>(ConfigAccountStore::load(write_accounts(duplicate))),
        std::invalid_argument);
}

TEST_F(AccountStoreTest, RejectsDuplicateAccountId) {
    constexpr std::string_view duplicate = R"lua(
return {
    accounts = {
        { account = "alpha", credential = "dev", account_id = 4242 },
        { account = "beta", credential = "dev", account_id = 4242 },
    },
}
)lua";
    EXPECT_THROW(
        static_cast<void>(ConfigAccountStore::load(write_accounts(duplicate))),
        std::invalid_argument);
}

TEST_F(AccountStoreTest, RejectsMissingCredentialField) {
    constexpr std::string_view missing = R"lua(
return { accounts = { { account = "player" } } }
)lua";
    EXPECT_THROW(
        static_cast<void>(ConfigAccountStore::load(write_accounts(missing))),
        std::invalid_argument);
}

TEST_F(AccountStoreTest, RejectsNonStringCredential) {
    constexpr std::string_view wrong_type = R"lua(
return { accounts = { { account = "player", credential = 123 } } }
)lua";
    EXPECT_THROW(
        static_cast<void>(ConfigAccountStore::load(write_accounts(wrong_type))),
        std::invalid_argument);
}

TEST_F(AccountStoreTest, RejectsNonPositiveAccountId) {
    constexpr std::string_view zero_id = R"lua(
return { accounts = { { account = "player", credential = "dev", account_id = 0 } } }
)lua";
    EXPECT_THROW(
        static_cast<void>(ConfigAccountStore::load(write_accounts(zero_id))),
        std::invalid_argument);
}

TEST_F(AccountStoreTest, RejectsMissingFile) {
    const auto missing = directory_.path() / "no-such-accounts.lua";
    EXPECT_THROW(static_cast<void>(ConfigAccountStore::load(missing)),
                 std::runtime_error);
}

TEST_F(AccountStoreTest, RejectsRootWithoutAccountsTable) {
    constexpr std::string_view no_accounts = R"lua(return { other = true })lua";
    EXPECT_THROW(
        static_cast<void>(
            ConfigAccountStore::load(write_accounts(no_accounts))),
        std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::common
