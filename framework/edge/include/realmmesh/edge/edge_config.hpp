#pragma once

// 接入面配置:EdgeRuntime 的三项构造输入(传输、运行时参数、来源地址)。
// 普通配置值，不带 runtime 布局，也不碰 Lua;解析在宿主的分层装载里。
#include "realmmesh/edge/edge_runtime_options.hpp"
#include "realmmesh/edge/edge_source_address.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

#include <cstddef>
#include <vector>

namespace realm::edge {

struct EdgeConfig {
    std::vector<network::TransportConfig> transports;
    EdgeRuntimeOptions runtime;
    EdgeSourceConfig ingress_source;

    /// 启用传输的 max_sessions 之和：本实例的连接额度。
    [[nodiscard]] std::size_t session_capacity() const noexcept {
        std::size_t capacity = 0;
        for (const auto& transport : transports) {
            if (transport.enabled) capacity += transport.max_sessions;
        }
        return capacity;
    }
};

}  // namespace realm::edge
