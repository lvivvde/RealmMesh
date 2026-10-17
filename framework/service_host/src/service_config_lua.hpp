#pragma once

// 宿主级与接入面节段的 Lua 解析(只供 layered_config_loader.cpp)。
#include "realmmesh/edge/edge_config.hpp"
#include "realmmesh/service_host/host_config.hpp"

#include <sol/forward.hpp>

namespace realm::service_host {

/// service_discovery 与 logging 节段;非法取值抛 std::invalid_argument。
[[nodiscard]] HostConfig parse_host_config(const sol::table& root);

/// transports(必须为表，可为空)、runtime 与 ingress_source 节段;启用的
/// 传输在此按配置或环境变量取 TLS 路径。非法取值抛 std::invalid_argument。
[[nodiscard]] edge::EdgeConfig parse_edge_config(const sol::table& root);

}  // namespace realm::service_host
