#pragma once

#include "realmmesh/network/transport/message_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace realm::cluster {

/// 服务身份。线名(注册 KV、额度路径、拓扑配置)是它的线上标识,已退役
/// 的线名永不复用;枚举数值不上线。
enum class ServiceType : std::uint8_t {
    Gateway,
    Realm,
    LoginVerify,
    Queue,
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
    Unavailable,
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

/// 服务类型的 etcd key 段名(注册 key 与额度 key 共用);未知类型抛
/// std::invalid_argument。
[[nodiscard]] std::string_view service_type_name(ServiceType type);

class IServiceRegistry {
public:
    virtual ~IServiceRegistry() = default;

    [[nodiscard]] virtual RegistrationResult register_instance(
        const ServiceInstance& instance, std::chrono::seconds lease_ttl) = 0;
    [[nodiscard]] virtual bool refresh_registration(
        RegistrationId registration_id) = 0;
    [[nodiscard]] virtual bool unregister_instance(
        RegistrationId registration_id) = 0;

    /// 把 key 挂到注册的租约上(与实例 key 同租约:实例死→key 消失)。
    /// Admission Budget 上报的载体(见主 spec §5.2)。注册中心记住成功
    /// 写入的 (key, value),refresh 重授租约改写实例 key 时同步改写;
    /// 写失败不改挂载表——新 key 不入表,已有 key 保留原值,由调用方
    /// 按最新值重试。未知注册返回 false。
    [[nodiscard]] virtual bool put_leased(
        RegistrationId registration_id,
        std::string_view key,
        std::string_view value) = 0;

    [[nodiscard]] virtual std::vector<ServiceInstance> discover(
        ServiceType type) const = 0;

    [[nodiscard]] virtual WatchId watch(
        ServiceType type, ServiceEventHandler handler) = 0;
    [[nodiscard]] virtual bool cancel_watch(WatchId watch_id) = 0;
};

}  // namespace realm::cluster
