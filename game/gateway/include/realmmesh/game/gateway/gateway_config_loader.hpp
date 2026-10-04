#pragma once

#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include <filesystem>

namespace realm::game::gateway {

/// 单文件装载 gateway 配置；只返回配置值，Lua 运行时不越出实现。
/// 已合并根表的解析入口见 gateway_config_lua.hpp。
class GatewayConfigLoader final {
public:
    [[nodiscard]] static GatewayConfig load(const std::filesystem::path& path);
};

}  // namespace realm::game::gateway
