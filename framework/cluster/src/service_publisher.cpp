#include "realmmesh/cluster/service_publisher.hpp"

#include <stdexcept>
#include <utility>

namespace realm::cluster {

ServicePublisher::ServicePublisher(
    IServiceRegistry& registry,
    ServiceInstance instance,
    std::chrono::seconds lease_ttl,
    std::chrono::seconds retry_interval)
    : registry_(registry),
      instance_(std::move(instance)),
      lease_ttl_(lease_ttl),
      retry_interval_(retry_interval) {
    if (lease_ttl_ <= std::chrono::seconds::zero() ||
        retry_interval_ <= std::chrono::seconds::zero()) {
        throw std::invalid_argument(
            "service publisher intervals must be positive");
    }
}

ServicePublisher::~ServicePublisher() {
    if (registration_id_ != invalid_registration_id) {
        static_cast<void>(registry_.unregister_instance(registration_id_));
    }
}

bool ServicePublisher::tick() {
    if (registered()) return true;
    const auto now = std::chrono::steady_clock::now();
    if (now < next_attempt_) return false;

    const auto result = registry_.register_instance(instance_, lease_ttl_);
    last_status_ = result.status;
    if (result.status == RegistryStatus::Success) {
        registration_id_ = result.id;
        return true;
    }
    next_attempt_ = now + retry_interval_;
    return false;
}

bool ServicePublisher::registered() const noexcept {
    return registration_id_ != invalid_registration_id;
}

RegistryStatus ServicePublisher::last_status() const noexcept {
    return last_status_;
}

}  // namespace realm::cluster
