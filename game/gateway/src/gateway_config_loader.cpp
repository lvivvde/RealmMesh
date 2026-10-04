#include "realmmesh/game/gateway/gateway_config_loader.hpp"

#include "realmmesh/game/gateway/gateway_config_lua.hpp"
#include "realmmesh/scripting/lua_runtime.hpp"

#include <stdexcept>
#include <string>

namespace realm::game::gateway {

// 运行时与根表都在本函数内创建和销毁，只有配置值越出(#126)。
GatewayConfig GatewayConfigLoader::load(const std::filesystem::path& path) {
    scripting::LuaRuntime runtime;
    std::string error;
    if (!runtime.load_module("gateway_config", path, &error)) {
        throw std::runtime_error(
            "failed to load gateway configuration: " + error);
    }
    const sol::table root = runtime.module("gateway_config");
    return parse_gateway_config(root);
}

}  // namespace realm::game::gateway
