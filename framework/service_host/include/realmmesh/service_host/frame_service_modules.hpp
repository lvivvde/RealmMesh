#pragma once

// Gateway 与 Realm 的服务模块(过渡形态):两者仍共用 ServiceFrame 业务帧,
// 暂留 framework/service_host;ADR-0014 第 5 步随业务帧迁入各自服务目录。
// 进程入口按服务名把工厂注册进 ServiceModuleRegistry。
#include "realmmesh/game/gateway/gateway_config.hpp"
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/service_host/layered_config.hpp"
#include "realmmesh/service_host/service_module.hpp"

#include <memory>

namespace realm::service_host {

/// gateway 节段 + player_data 路径解析;login.conn_capacity 取接入面启用
/// 传输的 max_sessions 之和。服务名为 gateway 时校验登录管线配置。
[[nodiscard]] game::gateway::GatewayConfig load_gateway_config(
    const LayeredConfig& config);

/// realm 节段;training_rule_file 相对路径按 config_root 解析。
[[nodiscard]] game::realm::RealmConfig load_realm_config(
    const LayeredConfig& config);

/// Gateway 模块:登录管线 + 准入 + 账号拉取;构造要求准入与身份密钥环境
/// 变量已设置。
[[nodiscard]] std::unique_ptr<ServiceModule> make_gateway_module(
    const ServiceModuleContext& context);

/// Realm 模块:直连入场 + Realm Session;要求 player_data.uri 与
/// REALMMESH_SESSION_TICKET_KEY。
[[nodiscard]] std::unique_ptr<ServiceModule> make_realm_module(
    const ServiceModuleContext& context);

}  // namespace realm::service_host
