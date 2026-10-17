#include "realmmesh/game/login_verify/login_verify_module.hpp"

#include "realmmesh/game/common/player_data_config.hpp"
#include "realmmesh/game/login_verify/login_verify_config_lua.hpp"
#include "realmmesh/game/login_verify/login_verify_service.hpp"
#include "realmmesh/observability/logger.hpp"
#include "realmmesh/service_host/layered_config_lua.hpp"

#include <utility>

namespace realm::game::login_verify {
namespace {

/// HTTPS 请求循环形态:无接入面 runtime,tick 驱动服务边轮询并补发验签
/// 响应。
class LoginVerifyModule final : public service_host::ServiceModule {
public:
    explicit LoginVerifyModule(
        const service_host::ServiceModuleContext& context)
        : logger_(&context.logger),
          service_(load_login_verify_config(context.config), &context.metrics) {
    }

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
        static_cast<void>(logger_->info(
            "service_stopped", "login_verify service stopped"));
    }

private:
    observability::Logger* logger_;
    LoginVerifyService service_;
};

}  // namespace

LoginVerifyConfig load_login_verify_config(
    const service_host::LayeredConfig& config) {
    auto login_verify =
        parse_login_verify_config(service_host::lua_root(config));
    login_verify.accounts_file = config.resolve_path(login_verify.accounts_file);
    common::resolve_player_data_paths(
        login_verify.player_data, config.config_root());
    return login_verify;
}

std::unique_ptr<service_host::ServiceModule> make_login_verify_module(
    const service_host::ServiceModuleContext& context) {
    return std::make_unique<LoginVerifyModule>(context);
}

}  // namespace realm::game::login_verify
