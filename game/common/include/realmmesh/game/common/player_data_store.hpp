#pragma once

#include "realmmesh/game/common/account_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace realm::game::common {

struct AccountProvisioning final {
    std::uint64_t account_id{0};
    std::string account_name;
    std::string credential;
    bool banned{false};
    bool whitelisted{false};
};

struct CharacterRecord final {
    std::uint64_t character_id{0};
    std::uint64_t account_id{0};
    std::uint32_t realm_id{0};
    std::string name;
    std::uint64_t revision{1};
};

struct AccountLoginFacts final {
    std::uint64_t account_id{0};
    std::uint64_t character_id{0};
    std::uint32_t realm_id{0};
    std::uint64_t character_revision{0};
};

class PlayerDataError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Gateway 与 Realm 共用的只读玩家数据边界。Gateway 读取可入场账号与
/// 当前选择角色，Realm 用票据中的三元组重新确认角色归属。
class PlayerDataReader {
public:
    virtual ~PlayerDataReader() = default;

    [[nodiscard]] virtual std::optional<AccountLoginFacts> login_facts(
        std::uint64_t account_id) const = 0;
    [[nodiscard]] virtual std::optional<CharacterRecord> character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) const = 0;
};

/// 新写入口令哈希的 Argon2 成本。校验参数取自哈希串本身，因此调整成本
/// 只影响之后写入的账号。Minimum 仅供批量机器人账号的测试/压测夹具。
enum class CredentialHashCost : std::uint8_t {
    Interactive,
    Minimum,
};

struct MongoPlayerDataOptions final {
    /// 找到可写 primary 的上限；超时即按数据源不可用失败。
    std::chrono::milliseconds server_selection_timeout{2'000};
    /// 单次网络读写的套接字超时，界定一次查询最多占用调用方多久。
    std::chrono::milliseconds socket_timeout{2'000};
    /// 非空时，连接后对尚未导入过的库执行一次性 Lua 导入（见
    /// bootstrap_from_lua）。三个服务共用同一配置，谁先到谁完成导入。
    std::optional<std::filesystem::path> bootstrap_accounts_file;
    CredentialHashCost credential_hash_cost{CredentialHashCost::Interactive};
};

/// 公共 `player_data` 配置段：三个服务读取同一份，指向同一个 MongoDB
/// 副本集与库（ADR-0011）。uri 为空表示未配置权威数据源（仅隔离测试使用）。
struct PlayerDataConfig final {
    std::string uri;
    std::string database;
    MongoPlayerDataOptions options;
};

/// 权威账号/角色存储（ADR-0011）。准入事实以 majority + journal 写入、
/// majority 读取，任一服务看到的都是已持久提交的最新事实；部署必须是
/// 副本集。线程安全：内部持有连接池，可被多个线程同时调用。
class MongoPlayerDataStore final : public AccountStore,
                                   public PlayerDataReader {
public:
    /// 连接失败、不是副本集或 schema 版本高于本二进制时抛 PlayerDataError。
    MongoPlayerDataStore(
        std::string uri,
        std::string database,
        MongoPlayerDataOptions options = {});
    ~MongoPlayerDataStore();

    MongoPlayerDataStore(const MongoPlayerDataStore&) = delete;
    MongoPlayerDataStore& operator=(const MongoPlayerDataStore&) = delete;

    [[nodiscard]] std::optional<AccountRecord> authenticate(
        std::string_view account,
        std::string_view credential) const override;
    [[nodiscard]] std::optional<AccountLoginFacts> login_facts(
        std::uint64_t account_id) const override;
    [[nodiscard]] std::optional<CharacterRecord> character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) const override;

    /// Provisioning 写路径；服务热路径只使用上面的只读边界。
    void provision_account(const AccountProvisioning& account);
    void set_account_access(
        std::uint64_t account_id, bool banned, bool whitelisted);
    void provision_character(const CharacterRecord& character);
    void select_character(
        std::uint64_t account_id, std::uint64_t character_id);
    /// 空库的一次性迁移入口。成功导入返回 true；库已有账号或已执行过
    /// bootstrap 时返回 false，绝不以 Lua 覆盖权威数据。
    [[nodiscard]] bool bootstrap_from_lua(
        const std::filesystem::path& accounts_file);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::game::common
