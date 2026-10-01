#pragma once

#include "realmmesh/game/common/account_store.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

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

struct SqlitePlayerDataOptions final {
    std::chrono::milliseconds busy_timeout{500};
    /// 非空时，打开库后对空库执行一次性 Lua 导入（见 bootstrap_from_lua）。
    /// 三个服务共用同一配置，谁先打开空库谁完成导入。
    std::optional<std::filesystem::path> bootstrap_accounts_file;
    CredentialHashCost credential_hash_cost{CredentialHashCost::Interactive};
};

/// 公共 `player_data` 配置段：三个服务读取同一份，指向同一个 SQLite 文件。
/// database_file 为空表示未配置权威数据源（仅隔离测试使用）。
struct PlayerDataConfig final {
    std::filesystem::path database_file;
    SqlitePlayerDataOptions options;
};

/// 单机部署的权威账号/角色存储。每次查询读取最新已提交事务；WAL 与
/// FULL synchronous 保证进程重启后已提交数据仍可恢复。
class SqlitePlayerDataStore final : public AccountStore,
                                    public PlayerDataReader {
public:
    explicit SqlitePlayerDataStore(
        std::filesystem::path database_file,
        SqlitePlayerDataOptions options = {});
    ~SqlitePlayerDataStore();

    SqlitePlayerDataStore(const SqlitePlayerDataStore&) = delete;
    SqlitePlayerDataStore& operator=(const SqlitePlayerDataStore&) = delete;

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
