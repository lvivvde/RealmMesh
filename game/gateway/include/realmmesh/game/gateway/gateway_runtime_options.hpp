#pragma once

// GatewayRuntime 的运行时参数：队列容量与 IO 循环节拍。单独成头，使
// gateway_runtime.hpp 不经 gateway_config.hpp 传递拿到登录配置与凭据入口
// (ADR-0014 第 2 步)。
#include <chrono>
#include <cstddef>

namespace realm::game::gateway {

struct GatewayRuntimeOptions {
    std::size_t inbound_capacity{65'536};
    std::size_t outbound_capacity{65'536};
    std::size_t max_commands_per_cycle{4'096};
    std::chrono::milliseconds io_poll_interval{2};
};

}  // namespace realm::game::gateway
