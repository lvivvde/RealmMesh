#pragma once

#include "realmmesh/cluster/service_registry.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace realm::cluster {

class ServiceResolver final {
public:
    ServiceResolver(
        IServiceRegistry& registry,
        ServiceType service_type,
        network::TransportProtocol protocol);
    ~ServiceResolver();

    ServiceResolver(const ServiceResolver&) = delete;
    ServiceResolver& operator=(const ServiceResolver&) = delete;

    [[nodiscard]] std::optional<network::TransportEndpoint> endpoint() const;

private:
    void apply(const ServiceEvent& event);

    IServiceRegistry& registry_;
    ServiceType service_type_;
    network::TransportProtocol protocol_;
    mutable std::mutex mutex_;
    std::map<std::string, ServiceInstance> instances_;
    WatchId watch_id_{invalid_watch_id};
};

}  // namespace realm::cluster
