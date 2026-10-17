#pragma once

// 模块解析自有节段的入口：只供服务模块的解析实现与解析测试显式包含;
// 包含方须自行链接 RealmMesh::Scripting。只用配置值的代码包含
// layered_config.hpp。
#include "realmmesh/service_host/layered_config.hpp"

#include <sol/forward.hpp>

namespace realm::service_host {

/// 合并后的 Lua 根表(公共层 + 服务层 + CLI 覆盖);与 config 同生命周期。
[[nodiscard]] const sol::table& lua_root(const LayeredConfig& config);

}  // namespace realm::service_host
