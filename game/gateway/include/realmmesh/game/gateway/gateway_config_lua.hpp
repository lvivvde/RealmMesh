#pragma once

// Lua 解析入口(#126):只供解析实现与服务模块装载显式选择;包含方须自行
// 链接 RealmMesh::Scripting。只用配置值的代码包含 gateway_config.hpp。
#include "realmmesh/game/gateway/gateway_config.hpp"

#include <sol/forward.hpp>

namespace realm::game::gateway {

/// 解析已合并 Lua 根表里的 gateway 节段(帧参数、下游、登录管线、准入、
/// player_data);非法取值抛 std::invalid_argument。login.conn_capacity
/// 来自接入面配置，由调用方填入后再 validate。
[[nodiscard]] GatewayConfig parse_gateway_config(const sol::table& root);

}  // namespace realm::game::gateway
