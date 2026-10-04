#pragma once

// Lua 解析入口(#126):只供解析实现与分层装载器显式选择；包含方须自行
// 链接 RealmMesh::Scripting。只用配置值的代码包含 login_verify_config.hpp。
#include "realmmesh/game/login_verify/login_verify_config.hpp"

#include <sol/forward.hpp>

namespace realm::game::login_verify {

/// 解析已合并的 Lua 根表；`login_verify` 节缺失或字段类型错抛
/// std::invalid_argument。
[[nodiscard]] LoginVerifyConfig parse_login_verify_config(const sol::table& root);

}  // namespace realm::game::login_verify
