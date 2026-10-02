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
#include <vector>

namespace realm::game::common {

struct AccountProvisioning final {
    std::uint64_t account_id{0};
    std::string account_name;
    std::string credential;
    bool banned{false};
    bool whitelisted{false};
};

/// Provisioning 写入的角色(测试夹具与运维导入);业务创建走
/// RealmCharacterStore::create_character。新角色经验与训练序号从 0 起。
struct CharacterRecord final {
    std::uint64_t character_id{0};
    std::uint64_t account_id{0};
    std::uint32_t realm_id{0};
    std::string name;
    std::uint64_t revision{1};
};

/// Gateway Fetching 的准入事实:只有未封禁且在白名单内的账号才有值
/// (ADR-0013:角色不参与准入)。
struct AccountLoginFacts final {
    std::uint64_t account_id{0};
};

class PlayerDataError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

/// Gateway 的只读准入边界:以 majority 读复核封禁与白名单。
class PlayerDataReader {
public:
    virtual ~PlayerDataReader() = default;

    [[nodiscard]] virtual std::optional<AccountLoginFacts> login_facts(
        std::uint64_t account_id) const = 0;
};

/// Realm 内一个角色的已确认状态(#93)。
struct RealmCharacter final {
    std::uint64_t character_id{0};
    std::string name;
    std::uint64_t exp{0};
    std::uint64_t last_training_seq{0};
};

/// 账号在某个 Realm 的角色列表,按创建先后排列。上次所选只在它属于本
/// Realm 的列表时给出,否则为 0。
struct CharacterRoster final {
    std::vector<RealmCharacter> characters;
    std::uint64_t last_selected_character_id{0};
};

enum class CreateCharacterOutcome : std::uint8_t {
    Created,
    /// 同一 Realm 内已有同名角色(按字节精确匹配,存储唯一索引保证)。
    NameTaken,
    /// 账号在该 Realm 的角色数已达上限。
    LimitReached,
};

struct CreateCharacterResult final {
    CreateCharacterOutcome outcome{CreateCharacterOutcome::Created};
    /// 仅 Created 时有意义。
    RealmCharacter character;
};

/// 一次训练的条件写:仅当已确认序号仍为 seq - 1 时写入新经验与 seq。
struct TrainingWrite final {
    std::uint64_t account_id{0};
    std::uint32_t realm_id{0};
    std::uint64_t character_id{0};
    std::uint64_t seq{0};
    std::uint64_t exp{0};
};

struct TrainingWriteResult final {
    /// false 表示条件不满足(序号已被别的写推进),character 为当前状态。
    bool committed{false};
    RealmCharacter character;
};

/// Realm Session 的角色数据边界(#93)。所有方法同步执行、可能阻塞于
/// 网络,只在工作线程调用;数据源故障抛 PlayerDataError。写入均为
/// majority + journal,并递增角色 revision。
class RealmCharacterStore {
public:
    virtual ~RealmCharacterStore() = default;

    [[nodiscard]] virtual CharacterRoster list_characters(
        std::uint64_t account_id, std::uint32_t realm_id) const = 0;
    /// name 由调用方预先校验;角色编号为随机非零 63 位,撞号重试。
    [[nodiscard]] virtual CreateCharacterResult create_character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::string_view name,
        std::size_t max_characters) = 0;
    /// 角色属于该账号与 Realm 时写入「上次所选」并返回其状态,否则 nullopt。
    [[nodiscard]] virtual std::optional<RealmCharacter> choose_character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) = 0;
    /// 角色不属于该账号与 Realm 时返回 nullopt。
    [[nodiscard]] virtual std::optional<TrainingWriteResult> record_training(
        const TrainingWrite& write) = 0;
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

/// 权威账号/角色存储(ADR-0011)。准入事实以 majority + journal 写入、
/// majority 读取，任一服务看到的都是已持久提交的最新事实；部署必须是
/// 副本集。线程安全：内部持有连接池，可被多个线程同时调用。
class MongoPlayerDataStore final : public AccountStore,
                                   public PlayerDataReader,
                                   public RealmCharacterStore {
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
    [[nodiscard]] CharacterRoster list_characters(
        std::uint64_t account_id, std::uint32_t realm_id) const override;
    [[nodiscard]] CreateCharacterResult create_character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::string_view name,
        std::size_t max_characters) override;
    [[nodiscard]] std::optional<RealmCharacter> choose_character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) override;
    /// 读单个角色(不属于该账号与 Realm 时 nullopt);不在 Realm 端口上,
    /// 供存储自身与测试核对落库状态。
    [[nodiscard]] std::optional<RealmCharacter> realm_character(
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id) const;
    [[nodiscard]] std::optional<TrainingWriteResult> record_training(
        const TrainingWrite& write) override;

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
