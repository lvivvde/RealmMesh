#pragma once

// LoginVerify 服务模块(ADR-0014):把 LoginVerifyService 接到通用
// ServiceHost。进程入口按服务名 login_verify 注册 make_login_verify_module。
#include "realmmesh/game/login_verify/login_verify_config.hpp"
#include "realmmesh/service_host/layered_config.hpp"
#include "realmmesh/service_host/service_module.hpp"

#include <memory>

namespace realm::game::login_verify {

/// 解析分层配置里的 login_verify 节段;accounts_file 与 player_data 的
/// 相对路径按 config_root 解析。非法取值抛 std::invalid_argument。
[[nodiscard]] LoginVerifyConfig load_login_verify_config(
    const service_host::LayeredConfig& config);

[[nodiscard]] std::unique_ptr<service_host::ServiceModule>
make_login_verify_module(const service_host::ServiceModuleContext& context);

}  // namespace realm::game::login_verify
