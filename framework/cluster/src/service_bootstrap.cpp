#include "realmmesh/cluster/service_bootstrap.hpp"

#include <utility>

namespace realm::cluster {

EtcdRegistryOptions make_etcd_registry_options(
    const ServiceDiscoveryConfig& config) {
    return {
        .endpoint = config.endpoint,
        .key_prefix = config.key_prefix,
        .request_timeout = config.request_timeout,
        .watch_interval = config.watch_interval,
        .background_maintenance = true,
    };
}

ServiceInstance make_service_instance(
    ServiceType type,
    const ServiceDiscoveryConfig& config,
    std::span<const network::TransportEndpoint> local_endpoints,
    std::string_view version) {
    ServiceInstance instance{
        .type = type,
        .instance_id = config.instance_id,
        .node_id = config.node_id,
        .zone = config.zone,
        .endpoints = {},
        .weight = 100,
        .version = std::string(version),
    };
    instance.endpoints.reserve(local_endpoints.size());
    for (const auto& local : local_endpoints) {
        auto advertised = local;
        advertised.address = config.advertise_address;
        instance.endpoints.push_back(std::move(advertised));
    }
    return instance;
}

}  // namespace realm::cluster
