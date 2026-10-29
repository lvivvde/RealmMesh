#pragma once

#include <atomic>
#include <cstdint>

namespace realm::service {

enum class ServiceState : std::uint8_t {
    Created,
    Initializing,
    Ready,
    Stopping,
    Stopped,
};

/// 以 CAS 串行化生命周期迁移；只允许
/// Created→Initializing→Ready/Stopping→Stopped，Stopped 为终态。
class ServiceLifecycle final {
public:
    ServiceLifecycle() = default;

    ServiceLifecycle(const ServiceLifecycle&) = delete;
    ServiceLifecycle& operator=(const ServiceLifecycle&) = delete;
    ServiceLifecycle(ServiceLifecycle&&) = delete;
    ServiceLifecycle& operator=(ServiceLifecycle&&) = delete;

    [[nodiscard]] ServiceState state() const noexcept;
    [[nodiscard]] bool transition_to(ServiceState next_state) noexcept;

private:
    [[nodiscard]] static bool is_valid_transition(
        ServiceState current_state, ServiceState next_state) noexcept;

    std::atomic<ServiceState> state_{ServiceState::Created};
};

}  // namespace realm::service
