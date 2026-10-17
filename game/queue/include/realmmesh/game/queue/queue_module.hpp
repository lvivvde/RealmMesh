#pragma once

// Queue 服务模块(ADR-0014):把 QueueService 接到通用 ServiceHost。进程
// 入口按服务名 queue 注册 make_queue_module。
#include "realmmesh/game/queue/queue_config.hpp"
#include "realmmesh/service_host/layered_config.hpp"
#include "realmmesh/service_host/service_module.hpp"

#include <memory>

namespace realm::game::queue {

/// 解析分层配置里的 queue 节段;非法取值抛 std::invalid_argument。
[[nodiscard]] QueueConfig load_queue_config(
    const service_host::LayeredConfig& config);

[[nodiscard]] std::unique_ptr<service_host::ServiceModule> make_queue_module(
    const service_host::ServiceModuleContext& context);

}  // namespace realm::game::queue
