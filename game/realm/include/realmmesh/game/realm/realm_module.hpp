#pragma once

// Realm 服务模块(ADR-0014):把直连入场与 Realm Session 接到通用
// ServiceHost 的接入面运行时上。进程入口按服务名 realm 注册
// make_realm_module。
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/service_host/layered_config.hpp"
#include "realmmesh/service_host/service_module.hpp"

#include <memory>

namespace realm::game::realm {

/// realm 节段与根表帧参数;training_rule_file 与 player_data 的相对路径
/// 按 config_root 解析。非法取值抛 std::invalid_argument。
[[nodiscard]] RealmConfig load_realm_config(
    const service_host::LayeredConfig& config);

/// 要求 player_data.uri 与 REALMMESH_SESSION_TICKET_KEY;训练规则加载失败
/// 即构造失败(#93)。
[[nodiscard]] std::unique_ptr<service_host::ServiceModule> make_realm_module(
    const service_host::ServiceModuleContext& context);

}  // namespace realm::game::realm
