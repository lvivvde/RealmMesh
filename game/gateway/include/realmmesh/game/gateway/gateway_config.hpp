#pragma once

// Gateway 启动配置(#128):宿主级节段与 runtime 选项的普通配置值，不碰
// Lua,也不带 GatewayRuntime 的私有布局。只用配置值的代码包含本头;单文件
// 装载见 gateway_config_loader.hpp,Lua 解析入口见 gateway_config_lua.hpp。
#include "realmmesh/cluster/service_discovery_config.hpp"
#include "realmmesh/game/common/player_data_config.hpp"
#include "realmmesh/game/gateway/gateway_ingress.hpp"
#include "realmmesh/game/gateway/gateway_login_config.hpp"
#include "realmmesh/network/transport/transport_config.hpp"
#include "realmmesh/observability/logger.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace realm::game::gateway {

struct GatewayRuntimeOptions {
    std::size_t inbound_capacity{65'536};
    std::size_t outbound_capacity{65'536};
    std::size_t max_commands_per_cycle{4'096};
    std::chrono::milliseconds io_poll_interval{2};
};

struct GatewayConfig {
    std::vector<network::TransportConfig> transports;
    GatewayRuntimeOptions runtime;
    cluster::ServiceDiscoveryConfig service_discovery;
    observability::LoggerConfig logging;
    observability::MetricsServerConfig logging_metrics;
    observability::ServiceIdentity logging_identity;
    std::uint32_t tick_rate{20};
    std::size_t max_events_per_frame{4'096};
    std::string downstream_address;
    std::uint16_t downstream_port{0};
    GatewayLoginConfig login;
    GatewaySourceConfig ingress_source;
    common::PlayerDataConfig player_data;
};

}  // namespace realm::game::gateway
