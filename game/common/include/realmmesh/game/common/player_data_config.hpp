#pragma once

#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/scripting/lua_runtime.hpp"

#include <filesystem>
#include <string_view>

namespace realm::game::common {

/// "interactive" | "minimum"；其他取值抛 std::invalid_argument。
[[nodiscard]] CredentialHashCost parse_credential_hash_cost(
    std::string_view value);

/// 解析根表中的可选 `player_data` 段；缺省返回空配置，类型或取值非法时
/// 抛 std::invalid_argument。
[[nodiscard]] PlayerDataConfig parse_player_data_config(const sol::table& root);

/// 相对路径（数据库与 bootstrap 文件）按 config_root 解析。
void resolve_player_data_paths(
    PlayerDataConfig& config, const std::filesystem::path& config_root);

}  // namespace realm::game::common
