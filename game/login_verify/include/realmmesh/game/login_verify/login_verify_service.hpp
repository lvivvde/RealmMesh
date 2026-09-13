#pragma once

#include "realmmesh/game/login_verify/login_verify_config.hpp"
#include "realmmesh/game/login_verify/login_verify_handler.hpp"
#include "realmmesh/network/transport/message_transport.hpp"

#include <memory>
#include <vector>

namespace realm::network {
class HttpServer;
}  // namespace realm::network

namespace realm::observability {
class Logger;
class MetricsRegistry;
}  // namespace observability

namespace realm::game::common {
class AccountStore;
class IdentityTokenCodec;
}  // namespace realm::game::common

namespace realm::game::login_verify {

/// 健全服运行体:HTTPS 服务边(#39)+ AccountStore(#40)+ 身份 Token
/// 编解码(#37)的装配。start 装载种子与账号集并绑定监听;tick 驱动
/// 服务边轮询,由 ServiceHost 每帧调用。
class LoginVerifyService final {
public:
    /// metrics(#47):宿主持有的指标注册表;可空(测试装配)。
    explicit LoginVerifyService(
        LoginVerifyConfig config,
        observability::MetricsRegistry* metrics = nullptr);
    ~LoginVerifyService();

    LoginVerifyService(const LoginVerifyService&) = delete;
    LoginVerifyService& operator=(const LoginVerifyService&) = delete;

    /// 装载 REALMMESH_IDENTITY_KEY_SEED、账号集并绑定监听;种子缺失或
    /// 账号集非法抛异常,成功返回即存活。logger 可为空(测试装配)。
    void start(observability::Logger* logger = nullptr);
    void stop();

    /// 驱动一轮服务边轮询;未启动时为空操作。
    void tick();

    [[nodiscard]] bool running() const noexcept;
    [[nodiscard]] const std::vector<network::TransportEndpoint>&
    local_endpoints() const noexcept;

private:
    LoginVerifyConfig config_;
    observability::MetricsRegistry* metrics_{nullptr};
    std::unique_ptr<common::AccountStore> store_;
    std::unique_ptr<common::IdentityTokenCodec> codec_;
    std::unique_ptr<LoginVerifyHandler> handler_;
    std::unique_ptr<network::HttpServer> server_;
    std::vector<network::TransportEndpoint> endpoints_;
};

}  // namespace realm::game::login_verify
