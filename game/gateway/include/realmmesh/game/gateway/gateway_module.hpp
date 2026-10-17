#pragma once

// Gateway 服务模块(ADR-0014):把登录管线、准入与账号拉取接到通用
// ServiceHost 的接入面运行时上。进程入口按服务名 gateway 注册
// make_gateway_module。
#include "realmmesh/game/gateway/gateway_config.hpp"
#include "realmmesh/service_host/layered_config.hpp"
#include "realmmesh/service_host/service_module.hpp"

#include <memory>

namespace realm::game::gateway {

/// gateway 节段 + player_data 路径解析;login.conn_capacity 取接入面启用
/// 传输的 max_sessions 之和,并校验登录管线配置。
[[nodiscard]] GatewayConfig load_gateway_config(
    const service_host::LayeredConfig& config);

/// 构造要求准入、身份与会话票据密钥环境变量已设置。
[[nodiscard]] std::unique_ptr<service_host::ServiceModule> make_gateway_module(
    const service_host::ServiceModuleContext& context);

}  // namespace realm::game::gateway
