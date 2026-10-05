#pragma once

// Lua 解析入口(#126):只供解析实现与分层装载器显式选择；包含方须自行
// 链接 RealmMesh::Scripting。只用配置值的代码包含 gateway_config.hpp。
#include "realmmesh/game/gateway/gateway_config.hpp"

#include <sol/forward.hpp>

namespace realm::game::gateway {

/// 解析已合并的 Lua 根表：宿主级节段(logging、service_discovery、
/// metrics)与 gateway 节段；非法取值抛 std::invalid_argument。
[[nodiscard]] GatewayConfig parse_gateway_config(const sol::table& root);

}  // namespace realm::game::gateway
