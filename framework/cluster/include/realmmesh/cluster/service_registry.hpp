#pragma once

#include "realmmesh/network/transport/message_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace realm::cluster {

enum class ServiceType : std::uint8_t {
    Coordinator,
    Gateway,
    Login,
    Lobby,
    Scene,
    Friend,
    Chat,
    Storage,
};

struct ServiceInstance {
    ServiceType type;
    std::string instance_id;
    std::string node_id;
    std::string zone;
    std::vector<network::TransportEndpoint> endpoints;
    std::uint32_t weight{100};
    std::string version;

    bool operator==(const ServiceInstance&) const = default;
};

using RegistrationId = std::uint64_t;
inline constexpr RegistrationId invalid_registration_id = 0;

using WatchId = std::uint64_t;
inline constexpr WatchId invalid_watch_id = 0;

enum class RegistryStatus {
    Success,
    AlreadyExists,
    NotFound,
    InvalidArgument,
};

struct RegistrationResult {
    RegistryStatus status;
    RegistrationId id{invalid_registration_id};
};

enum class ServiceEventKind {
    Added,
    Removed,
};

struct ServiceEvent {
    ServiceEventKind kind;
    ServiceInstance instance;

    bool operator==(const ServiceEvent&) const = default;
};

using ServiceEventHandler = std::function<void(const ServiceEvent&)>;

class IServiceRegistry {
public:
    virtual ~IServiceRegistry() = default;

    [[nodiscard]] virtual RegistrationResult register_instance(
        const ServiceInstance& instance,
        std::chrono::seconds lease_ttl) = 0;
    [[nodiscard]] virtual bool refresh_registration(RegistrationId registration_id) = 0;
    [[nodiscard]] virtual bool unregister_instance(RegistrationId registration_id) = 0;

    [[nodiscard]] virtual std::vector<ServiceInstance> discover(
        ServiceType type) const = 0;

    [[nodiscard]] virtual WatchId watch(
        ServiceType type,
        ServiceEventHandler handler) = 0;
    [[nodiscard]] virtual bool cancel_watch(WatchId watch_id) = 0;
};

}  // namespace realm::cluster
