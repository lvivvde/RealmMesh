#pragma once

#include "realmmesh/cluster/service_registry.hpp"

#include <chrono>

namespace realm::cluster {

class ServicePublisher final {
public:
    ServicePublisher(
        IServiceRegistry& registry,
        ServiceInstance instance,
        std::chrono::seconds lease_ttl,
        std::chrono::seconds retry_interval = std::chrono::seconds(2));
    ~ServicePublisher();

    ServicePublisher(const ServicePublisher&) = delete;
    ServicePublisher& operator=(const ServicePublisher&) = delete;

    [[nodiscard]] bool tick();
    [[nodiscard]] bool registered() const noexcept;
    [[nodiscard]] RegistryStatus last_status() const noexcept;

private:
    IServiceRegistry& registry_;
    ServiceInstance instance_;
    std::chrono::seconds lease_ttl_;
    std::chrono::seconds retry_interval_;
    RegistrationId registration_id_{invalid_registration_id};
    RegistryStatus last_status_{RegistryStatus::Unavailable};
    std::chrono::steady_clock::time_point next_attempt_{};
};

}  // namespace realm::cluster
