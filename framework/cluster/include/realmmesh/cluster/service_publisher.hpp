#pragma once

#include "realmmesh/cluster/service_registry.hpp"

#include <chrono>

namespace realm::cluster {

/// 将单个实例注册到发现中心的重试状态机。registry 为非拥有引用且必须比本对象
/// 活得更久；Lease 的后台续期由具体 registry 实现负责。
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

    /// 已注册时返回 true；失败后在 retry_interval 内不会再次发起请求。
    [[nodiscard]] bool tick();
    [[nodiscard]] bool registered() const noexcept;
    /// 当前注册标识;未注册时为 invalid_registration_id(额度上报等
    /// 挂租约写以此为准,需在每次使用时重读而非缓存)。
    [[nodiscard]] RegistrationId registration_id() const noexcept {
        return registration_id_;
    }
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
