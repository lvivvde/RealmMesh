#pragma once

// 宿主级配置(ADR-0014):每个服务都有的节段——服务发现与日志(含指标端口
// 与日志身份)。普通配置值，不碰 Lua。
#include "realmmesh/cluster/service_discovery_config.hpp"
#include "realmmesh/observability/logger.hpp"

namespace realm::service_host {

struct HostConfig {
    cluster::ServiceDiscoveryConfig service_discovery;
    observability::LoggerConfig logging;
    observability::MetricsServerConfig logging_metrics;
    observability::ServiceIdentity logging_identity;
};

}  // namespace realm::service_host
