#include "realmmesh/cluster/service_resolver.hpp"

#include <algorithm>

namespace realm::cluster {

ServiceResolver::ServiceResolver(
    IServiceRegistry& registry,
    ServiceType service_type,
    network::TransportProtocol protocol)
    : registry_(registry), service_type_(service_type), protocol_(protocol) {
    watch_id_ = registry_.watch(
        service_type_, [this](const ServiceEvent& event) { apply(event); });
}

ServiceResolver::~ServiceResolver() {
    if (watch_id_ != invalid_watch_id) {
        static_cast<void>(registry_.cancel_watch(watch_id_));
    }
}

std::optional<network::TransportEndpoint> ServiceResolver::endpoint() const {
    const std::scoped_lock lock(mutex_);
    const network::TransportEndpoint* selected = nullptr;
    std::uint32_t selected_weight = 0;
    for (const auto& [instance_id, instance] : instances_) {
        static_cast<void>(instance_id);
        const auto endpoint = std::ranges::find_if(
            instance.endpoints,
            [this](const network::TransportEndpoint& candidate) {
                return candidate.protocol == protocol_;
            });
        if (endpoint != instance.endpoints.end() &&
            (selected == nullptr || instance.weight > selected_weight)) {
            selected = &*endpoint;
            selected_weight = instance.weight;
        }
    }
    if (selected == nullptr) return std::nullopt;
    return *selected;
}

void ServiceResolver::apply(const ServiceEvent& event) {
    const std::scoped_lock lock(mutex_);
    if (event.kind == ServiceEventKind::Added) {
        instances_.insert_or_assign(event.instance.instance_id, event.instance);
    } else {
        instances_.erase(event.instance.instance_id);
    }
}

}  // namespace realm::cluster
