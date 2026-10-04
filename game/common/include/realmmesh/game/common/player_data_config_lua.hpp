#pragma once

// Lua 解析入口(#126):只供解析实现与分层装载器显式选择；包含方须自行
// 链接 RealmMesh::Scripting。只用配置值的代码包含 player_data_config.hpp。
#include "realmmesh/game/common/player_data_config.hpp"

#include <sol/forward.hpp>

namespace realm::game::common {

/// 解析根表中的可选 `player_data` 段；缺省返回空配置，类型或取值非法时
/// 抛 std::invalid_argument。
[[nodiscard]] PlayerDataConfig parse_player_data_config(const sol::table& root);

}  // namespace realm::game::common
