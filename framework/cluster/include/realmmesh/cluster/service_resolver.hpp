#pragma once

#include "realmmesh/cluster/service_registry.hpp"

#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace realm::cluster {

class ServiceResolver final {
public:
    ServiceResolver(
        IServiceRegistry& registry,
        ServiceType service_type,
        network::TransportProtocol protocol);
    ~ServiceResolver();

    ServiceResolver(const ServiceResolver&) = delete;
    ServiceResolver& operator=(const ServiceResolver&) = delete;

    [[nodiscard]] std::optional<network::TransportEndpoint> endpoint() const;
    [[nodiscard]] std::optional<network::TransportEndpoint> endpoint(
        std::string_view instance_id) const;
    /// 一次完整发现快照的端点集合；初始化失败/差分发布中为 nullopt，
    /// 不能把未知当成权威空列表。仅拷贝已交付缓存，不执行网络 IO。
    [[nodiscard]] std::optional<
        std::map<std::string, network::TransportEndpoint>>
    endpoints() const;

private:
    void apply(const ServiceEvent& event);

    IServiceRegistry& registry_;
    ServiceType service_type_;
    network::TransportProtocol protocol_;
    mutable std::mutex mutex_;
    std::map<std::string, ServiceInstance> instances_;
    WatchId watch_id_{invalid_watch_id};
};

}  // namespace realm::cluster
