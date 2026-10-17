#include "realmmesh/game/queue/queue_module.hpp"

#include "realmmesh/game/queue/queue_config_lua.hpp"
#include "realmmesh/game/queue/queue_service.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/service_host/layered_config_lua.hpp"

#include <utility>

namespace realm::game::queue {
namespace {

/// HTTPS 请求循环形态:无接入面 runtime,tick 驱动服务边轮询与放行帧。
class QueueModule final : public service_host::ServiceModule {
public:
    explicit QueueModule(const service_host::ServiceModuleContext& context)
        : logger_(&context.logger),
          service_(load_queue_config(context.config), &context.metrics) {}

    void start() override { service_.start(logger_); }
    void stop() override { service_.stop(); }
    [[nodiscard]] bool running() const noexcept override {
        return service_.running();
    }
    [[nodiscard]] std::span<const network::TransportEndpoint> local_endpoints()
        const noexcept override {
        return service_.local_endpoints();
    }
    void tick(const service_host::ServiceTickContext&) override {
        service_.tick();
    }
    void stopped() override {
        static_cast<void>(
            logger_->info("service_stopped", "queue service stopped"));
    }

private:
    observability::Logger* logger_;
    QueueService service_;
};

}  // namespace

QueueConfig load_queue_config(const service_host::LayeredConfig& config) {
    return parse_queue_config(service_host::lua_root(config));
}

std::unique_ptr<service_host::ServiceModule> make_queue_module(
    const service_host::ServiceModuleContext& context) {
    return std::make_unique<QueueModule>(context);
}

}  // namespace realm::game::queue
