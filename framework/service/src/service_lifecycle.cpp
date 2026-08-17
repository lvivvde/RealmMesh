#include "realmmesh/service/service_lifecycle.hpp"

#include <atomic>

namespace realm::service {

ServiceState ServiceLifecycle::state() const noexcept {
    return state_.load(std::memory_order_acquire);
}

bool ServiceLifecycle::transition_to(ServiceState next_state) noexcept {
    auto current_state = state_.load(std::memory_order_acquire);

    while (is_valid_transition(current_state, next_state)) {
        if (state_.compare_exchange_weak(
                current_state,
                next_state,
                std::memory_order_acq_rel,
                std::memory_order_acquire)) {
            return true;
        }
    }

    return false;
}

bool ServiceLifecycle::is_valid_transition(
    ServiceState current_state,
    ServiceState next_state) noexcept {
    switch (current_state) {
    case ServiceState::Created:
        return next_state == ServiceState::Initializing;
    case ServiceState::Initializing:
        return next_state == ServiceState::Ready || next_state == ServiceState::Stopping;
    case ServiceState::Ready:
        return next_state == ServiceState::Stopping;
    case ServiceState::Stopping:
        return next_state == ServiceState::Stopped;
    case ServiceState::Stopped:
        return false;
    }

    return false;
}

}  // namespace realm::service
