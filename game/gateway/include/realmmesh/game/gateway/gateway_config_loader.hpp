#pragma once

#include "realmmesh/game/gateway/gateway_server.hpp"
#include "realmmesh/scripting/lua_runtime.hpp"

#include <filesystem>

namespace realm::game::gateway {

class GatewayConfigLoader final {
public:
    /// 从文件加载并解析(行为不变)。
    [[nodiscard]] static GatewayConfig load(const std::filesystem::path& path);

    /// 直接解析已合并的 Lua 根表(供分层加载器复用)。
    [[nodiscard]] static GatewayConfig parse(const sol::table& root);
};

}  // namespace realm::game::gateway
