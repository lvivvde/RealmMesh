#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace realm::game::common {

/// 账号记录:数据源给出的账号事实。封禁/白名单只是数据;拒绝档位
/// (1001/1002/1003)与模糊拒绝策略由消费方(登录健全服)决定。
struct AccountRecord {
    std::uint64_t account_id{0};
    bool banned{false};
    bool whitelisted{false};
};

/// 账号有效性数据源抽象(登录链路规格 §3):只回答「口令是否命中」。
/// 未知账号与口令错误一律返回空——账号存在性不外泄,统一按凭据无效处理。
/// TODO:DB 真源、Redis 起服预热以新实现接入,调用方零改动;
/// 模糊拒绝开关属消费方策略(见 AccountRecord 注释),同样不改本接口。
class AccountStore {
public:
    virtual ~AccountStore() = default;

    [[nodiscard]] virtual std::optional<AccountRecord> authenticate(
        std::string_view account, std::string_view credential) const = 0;
};

/// v1 数据源:Lua 配置装载的内存账号表(开发账号集)。
class ConfigAccountStore final : public AccountStore {
public:
    /// 从 Lua 文件装载:重复账号名/重复 id、字段缺失或类型错即失败。
    [[nodiscard]] static ConfigAccountStore load(
        const std::filesystem::path& path);

    [[nodiscard]] std::optional<AccountRecord> authenticate(
        std::string_view account, std::string_view credential) const override;

private:
    struct Entry {
        std::string credential;
        AccountRecord record;
    };

    std::unordered_map<std::string, Entry> accounts_;
};

}  // namespace realm::game::common
