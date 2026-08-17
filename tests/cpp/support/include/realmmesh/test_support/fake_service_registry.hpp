#pragma once

#include "realmmesh/cluster/service_registry.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace realm::test_support {

class FakeServiceRegistry final : public cluster::IServiceRegistry {
public:
    [[nodiscard]] cluster::RegistrationResult register_instance(
        const cluster::ServiceInstance& instance,
        std::chrono::seconds lease_ttl) override;
    [[nodiscard]] bool refresh_registration(
        cluster::RegistrationId registration_id) override;
    [[nodiscard]] bool unregister_instance(
        cluster::RegistrationId registration_id) override;

    [[nodiscard]] std::vector<cluster::ServiceInstance> discover(
        cluster::ServiceType type) const override;

    [[nodiscard]] cluster::WatchId watch(
        cluster::ServiceType type,
        cluster::ServiceEventHandler handler) override;
    [[nodiscard]] bool cancel_watch(cluster::WatchId watch_id) override;

    [[nodiscard]] bool expire_registration(
        cluster::RegistrationId registration_id);

private:
    struct Registration {
        cluster::ServiceInstance instance;
        std::chrono::seconds lease_ttl;
    };

    struct Watch {
        cluster::ServiceType type;
        cluster::ServiceEventHandler handler;
    };

    using ServiceKey = std::pair<cluster::ServiceType, std::string>;

    [[nodiscard]] bool remove_registration(
        cluster::RegistrationId registration_id);
    [[nodiscard]] std::vector<cluster::ServiceEventHandler> handlers_for_locked(
        cluster::ServiceType type) const;

    mutable std::mutex mutex_;
    cluster::RegistrationId next_registration_id_{1};
    cluster::WatchId next_watch_id_{1};
    std::unordered_map<cluster::RegistrationId, Registration> registrations_;
    std::map<ServiceKey, cluster::RegistrationId> service_keys_;
    std::unordered_map<cluster::WatchId, Watch> watches_;
};

}  // namespace realm::test_support
