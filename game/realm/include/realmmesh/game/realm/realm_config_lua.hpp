#pragma once

// Lua 解析入口(#126):只供解析实现与分层装载器显式选择；包含方须自行
// 链接 RealmMesh::Scripting。只用配置值的代码包含 realm_config.hpp。
#include "realmmesh/game/realm/realm_config.hpp"

#include <sol/forward.hpp>

namespace realm::game::realm {

/// 解析已合并的 Lua 根表；`realm` 节或 training_rule_file 缺失、字段
/// 类型错误时抛 std::invalid_argument。
[[nodiscard]] RealmConfig parse_realm_config(const sol::table& root);

}  // namespace realm::game::realm
