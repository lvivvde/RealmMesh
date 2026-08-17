#pragma once

#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_discovery_config.hpp"

#include <span>
#include <string_view>

namespace realm::cluster {

[[nodiscard]] EtcdRegistryOptions make_etcd_registry_options(
    const ServiceDiscoveryConfig& config);

[[nodiscard]] ServiceInstance make_service_instance(
    ServiceType type,
    const ServiceDiscoveryConfig& config,
    std::span<const network::TransportEndpoint> local_endpoints,
    std::string_view version);

}  // namespace realm::cluster
