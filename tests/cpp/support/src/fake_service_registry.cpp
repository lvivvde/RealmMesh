#include "realmmesh/test_support/fake_service_registry.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

namespace realm::test_support {
namespace {

bool is_valid_instance(const cluster::ServiceInstance& instance) {
    return !instance.instance_id.empty() &&
           !instance.node_id.empty() &&
           !instance.zone.empty() &&
           !instance.endpoints.empty() &&
           !instance.endpoints.front().address.empty() &&
           instance.endpoints.front().port != 0;
}

void publish_event(
    const cluster::ServiceEvent& event,
    const std::vector<cluster::ServiceEventHandler>& handlers) {
    for (const auto& handler : handlers) {
        handler(event);
    }
}

}  // namespace

cluster::RegistrationResult FakeServiceRegistry::register_instance(
    const cluster::ServiceInstance& instance,
    std::chrono::seconds lease_ttl) {
    if (!is_valid_instance(instance) || lease_ttl <= std::chrono::seconds::zero()) {
        return {cluster::RegistryStatus::InvalidArgument};
    }

    cluster::RegistrationId registration_id = cluster::invalid_registration_id;
    std::vector<cluster::ServiceEventHandler> handlers;
    {
        const std::scoped_lock lock(mutex_);
        const ServiceKey key{instance.type, instance.instance_id};
        if (service_keys_.contains(key)) {
            return {cluster::RegistryStatus::AlreadyExists};
        }

        registration_id = next_registration_id_++;
        registrations_.emplace(
            registration_id,
            Registration{instance, lease_ttl});
        service_keys_.emplace(key, registration_id);
        handlers = handlers_for_locked(instance.type);
    }

    publish_event(
        {cluster::ServiceEventKind::Added, instance},
        handlers);
    return {cluster::RegistryStatus::Success, registration_id};
}

bool FakeServiceRegistry::refresh_registration(
    cluster::RegistrationId registration_id) {
    const std::scoped_lock lock(mutex_);
    return registrations_.contains(registration_id);
}

bool FakeServiceRegistry::unregister_instance(
    cluster::RegistrationId registration_id) {
    return remove_registration(registration_id);
}

std::vector<cluster::ServiceInstance> FakeServiceRegistry::discover(
    cluster::ServiceType type) const {
    std::vector<cluster::ServiceInstance> instances;
    {
        const std::scoped_lock lock(mutex_);
        for (const auto& [registration_id, registration] : registrations_) {
            static_cast<void>(registration_id);
            if (registration.instance.type == type) {
                instances.push_back(registration.instance);
            }
        }
    }

    std::ranges::sort(instances, {}, &cluster::ServiceInstance::instance_id);
    return instances;
}

cluster::WatchId FakeServiceRegistry::watch(
    cluster::ServiceType type,
    cluster::ServiceEventHandler handler) {
    if (!handler) {
        return cluster::invalid_watch_id;
    }

    const std::scoped_lock lock(mutex_);
    const auto watch_id = next_watch_id_++;
    watches_.emplace(watch_id, Watch{type, std::move(handler)});
    return watch_id;
}

bool FakeServiceRegistry::cancel_watch(cluster::WatchId watch_id) {
    const std::scoped_lock lock(mutex_);
    return watches_.erase(watch_id) == 1;
}

bool FakeServiceRegistry::expire_registration(
    cluster::RegistrationId registration_id) {
    return remove_registration(registration_id);
}

bool FakeServiceRegistry::remove_registration(
    cluster::RegistrationId registration_id) {
    cluster::ServiceInstance instance;
    std::vector<cluster::ServiceEventHandler> handlers;
    {
        const std::scoped_lock lock(mutex_);
        const auto registration = registrations_.find(registration_id);
        if (registration == registrations_.end()) {
            return false;
        }

        instance = registration->second.instance;
        service_keys_.erase({instance.type, instance.instance_id});
        registrations_.erase(registration);
        handlers = handlers_for_locked(instance.type);
    }

    publish_event(
        {cluster::ServiceEventKind::Removed, instance},
        handlers);
    return true;
}

std::vector<cluster::ServiceEventHandler>
FakeServiceRegistry::handlers_for_locked(cluster::ServiceType type) const {
    std::vector<cluster::ServiceEventHandler> handlers;
    for (const auto& [watch_id, watch] : watches_) {
        static_cast<void>(watch_id);
        if (watch.type == type) {
            handlers.push_back(watch.handler);
        }
    }
    return handlers;
}

}  // namespace realm::test_support
