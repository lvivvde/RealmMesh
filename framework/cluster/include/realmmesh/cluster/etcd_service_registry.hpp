#pragma once

#include "realmmesh/cluster/service_registry.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace realm::cluster {

struct EtcdRegistryOptions {
    std::string endpoint{"http://127.0.0.1:2379"};
    std::string key_prefix{"/realmmesh/services"};
    std::chrono::milliseconds request_timeout{500};
    std::chrono::milliseconds watch_interval{500};
    bool background_maintenance{true};
};

class IEtcdHttpClient {
public:
    virtual ~IEtcdHttpClient() = default;

    [[nodiscard]] virtual std::optional<std::string> post(
        std::string_view path,
        std::string_view json_body,
        std::string* error) = 0;
};

class EtcdServiceRegistry final : public IServiceRegistry {
public:
    explicit EtcdServiceRegistry(EtcdRegistryOptions options = {});
    EtcdServiceRegistry(
        EtcdRegistryOptions options,
        std::shared_ptr<IEtcdHttpClient> http_client);
    ~EtcdServiceRegistry() override;

    EtcdServiceRegistry(const EtcdServiceRegistry&) = delete;
    EtcdServiceRegistry& operator=(const EtcdServiceRegistry&) = delete;

    [[nodiscard]] RegistrationResult register_instance(
        const ServiceInstance& instance,
        std::chrono::seconds lease_ttl) override;
    [[nodiscard]] bool refresh_registration(
        RegistrationId registration_id) override;
    [[nodiscard]] bool unregister_instance(
        RegistrationId registration_id) override;
    [[nodiscard]] std::vector<ServiceInstance> discover(
        ServiceType type) const override;
    [[nodiscard]] WatchId watch(
        ServiceType type,
        ServiceEventHandler handler) override;
    [[nodiscard]] bool cancel_watch(WatchId watch_id) override;

    void poll_once();
    [[nodiscard]] std::string last_error() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::cluster
