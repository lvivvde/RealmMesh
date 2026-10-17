#pragma once

// 接入面服务模块:持有按 LayeredConfig::edge() 构造的 EdgeRuntime,帧循环
// 空转。消息形态的服务在其上派生业务帧;本头只前置声明 EdgeRuntime。
#include "realmmesh/service_host/service_module.hpp"

#include <memory>
#include <optional>
#include <span>
#include <string>

namespace realm::edge {
class EdgeRuntime;
}  // namespace realm::edge

namespace realm::service_host {

class EdgeServiceModule : public ServiceModule {
public:
    explicit EdgeServiceModule(const ServiceModuleContext& context);
    ~EdgeServiceModule() override;

    EdgeServiceModule(const EdgeServiceModule&) = delete;
    EdgeServiceModule& operator=(const EdgeServiceModule&) = delete;

    void start() override;
    void stop() override;
    [[nodiscard]] bool running() const noexcept override;
    [[nodiscard]] std::span<const network::TransportEndpoint> local_endpoints()
        const noexcept override;
    void tick(const ServiceTickContext& context) override;
    [[nodiscard]] std::optional<std::string> terminal_error() const override;

    [[nodiscard]] edge::EdgeRuntime& runtime() noexcept;
    [[nodiscard]] const edge::EdgeRuntime& runtime() const noexcept;

protected:
    [[nodiscard]] observability::Logger& logger() const noexcept;
    [[nodiscard]] const std::string& service_name() const noexcept;

private:
    observability::Logger* logger_;
    std::string service_name_;
    std::unique_ptr<edge::EdgeRuntime> runtime_;
};

}  // namespace realm::service_host
