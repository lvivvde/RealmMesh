#pragma once

#include "realmmesh/concurrency/bounded_work_pool.hpp"
#include "realmmesh/game/login_verify/login_verify_handler.hpp"
#include "realmmesh/network/http/http_server.hpp"

#include <chrono>
#include <cstddef>
#include <vector>

namespace realm::game::login_verify {

/// 验签工作者满额时的建议退避(Retry-After 头与 retry_after_seconds)。
inline constexpr std::chrono::seconds verifier_busy_retry_after{1};

/// 验签请求的异步分派(#98,ADR-0007 补记):POST /v1/login/verify 的认定
/// (Argon2 + 账号源查询)交给有界工作者,HttpServer 挂起该响应;其余路由
/// 仍在 loop 线程同步应答。工作者满额即回 503 + Retry-After(1005),
/// 不排进无界队列。属主线程每帧 drain 已完成的响应,交 HttpServer::complete。
class LoginVerifyDispatcher final {
public:
    struct Limits final {
        std::size_t workers{4};
        /// 排队 + 运行中 + 已完成未取走的验签总数上限。
        std::size_t capacity{64};
    };

    struct Completion final {
        network::HttpResponseToken token{0};
        network::Http1Response response;
    };

    /// handler 须比本对象活得久;其 handle 在工作线程并发调用(账号源与
    /// 指标注册表均为线程安全)。workers/capacity 为 0 抛 std::invalid_argument。
    LoginVerifyDispatcher(const LoginVerifyHandler& handler, Limits limits);

    LoginVerifyDispatcher(const LoginVerifyDispatcher&) = delete;
    LoginVerifyDispatcher& operator=(const LoginVerifyDispatcher&) = delete;

    [[nodiscard]] network::HttpHandlerResult dispatch(
        const network::Http1Request& request,
        network::HttpResponseToken token);

    [[nodiscard]] std::vector<Completion> drain(std::size_t max_items);

private:
    const LoginVerifyHandler* handler_;
    concurrency::BoundedWorkPool<network::Http1Response> pool_;
};

}  // namespace realm::game::login_verify
