#pragma once

// realm_mesh 进程的服务清单(ADR-0014):服务名 → 模块工厂的唯一装配点。
// framework 不点名任何游戏服务;集成测试经本库得到与生产相同的注册表。
#include "realmmesh/service_host/service_module.hpp"

namespace realm::mesh_host {

/// gateway(依赖 realm,模式 2 等待其可连)、realm(依赖 gateway)、
/// login_verify、queue。
[[nodiscard]] service_host::ServiceModuleRegistry game_service_modules();

}  // namespace realm::mesh_host
