#pragma once

#include "realmmesh/cluster/service_registry.hpp"

#include <chrono>
#include <map>
#include <mutex>
#include <string>
#include <string_view>
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

    [[nodiscard]] bool put_leased(
        cluster::RegistrationId registration_id,
        std::string_view key,
        std::string_view value) override;

    [[nodiscard]] bool expire_registration(
        cluster::RegistrationId registration_id);

    /// 挂到注册租约上的 key(put_leased 的观察口);未知注册返回空表。
    [[nodiscard]] std::map<std::string, std::string> leased_keys(
        cluster::RegistrationId registration_id) const;

private:
    struct Registration {
        cluster::ServiceInstance instance;
        std::chrono::seconds lease_ttl;
        std::map<std::string, std::string> leased_keys;
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
