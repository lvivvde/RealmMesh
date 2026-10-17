#pragma once

#include "realmmesh/network/transport/message_transport.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace realm::cluster {

/// 服务身份:即服务名(注册 KV、额度路径、拓扑配置共用的线名)。
/// framework 不枚举具体身份,权威列表是组合根的模块注册表
/// (ADR-0014);已退役的线名永不复用。
using ServiceType = std::string;

/// 合法身份:非空,只含小写字母、数字与下划线(可直接作 etcd key 段)。
[[nodiscard]] bool is_valid_service_type(std::string_view type) noexcept;

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
        const ServiceType& type) const = 0;

    [[nodiscard]] virtual WatchId watch(
        const ServiceType& type, ServiceEventHandler handler) = 0;
    [[nodiscard]] virtual bool cancel_watch(WatchId watch_id) = 0;
};

}  // namespace realm::cluster
