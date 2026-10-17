#pragma once

// EdgeRuntime 的运行时参数：队列容量与 IO 循环节拍。单独成头，供服务配置
// 只声明参数值而不带入运行时布局。
#include <chrono>
#include <cstddef>

namespace realm::edge {

struct EdgeRuntimeOptions {
    std::size_t inbound_capacity{65'536};
    std::size_t outbound_capacity{65'536};
    std::size_t max_commands_per_cycle{4'096};
    std::chrono::milliseconds io_poll_interval{2};
};

}  // namespace realm::edge
