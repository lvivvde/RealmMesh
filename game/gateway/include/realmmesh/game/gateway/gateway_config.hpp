#pragma once

// Gateway 节段配置(#128、ADR-0014):帧参数、下游与登录管线的普通配置值,
// 不碰 Lua。服务发现、日志与接入面节段由宿主分层装载解析(HostConfig、
// edge::EdgeConfig);Lua 解析入口见 gateway_config_lua.hpp。
#include "realmmesh/game/common/player_data_config.hpp"
#include "realmmesh/game/gateway/gateway_login_config.hpp"

#include <cstddef>
#include <cstdint>
#include <string>

namespace realm::game::gateway {

struct GatewayConfig {
    std::uint32_t tick_rate{20};
    std::size_t max_events_per_frame{4'096};
    std::string downstream_address;
    std::uint16_t downstream_port{0};
    GatewayLoginConfig login;
    common::PlayerDataConfig player_data;
};

}  // namespace realm::game::gateway
