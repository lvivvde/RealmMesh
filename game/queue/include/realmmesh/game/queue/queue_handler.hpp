#pragma once

#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/queue/queue_ticketing.hpp"
#include "realmmesh/network/http/http1_parser.hpp"
#include "realmmesh/network/http/http1_response.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace realm::observability {
class MetricsRegistry;
}  // namespace observability

namespace realm::game::queue {

/// 排队 HTTP 路由(§5.1 行 2/3/4 + /healthz):POST /v1/queue/tickets
/// (Bearer 身份 Token,同 jti 幂等)、GET /v1/queue/progress(CDN 可
/// 缓存)、GET /v1/queue/tickets/me(Bearer Queue Number v2,放行即签发
/// 身份绑定的 Admission Grant)。
/// 错误模型 {code, message}:1001 身份凭据无效(1xxx 沿用 edge.proto)、
/// 2001 号牌无效/过期、1000 其余请求性错误。
class QueueHandler final {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    static constexpr int error_invalid_request = 1000;
    static constexpr int error_invalid_credentials = 1001;
    static constexpr int error_invalid_number = 2001;

    QueueHandler(
        QueueTicketing& ticketing,
        const common::IdentityTokenCodec& identity_codec,
        Clock clock,
        std::string_view identity_issuer,
        observability::MetricsRegistry* metrics = nullptr);

    [[nodiscard]] network::Http1Response handle(
        const network::Http1Request& request) const;

private:
    QueueTicketing* ticketing_;
    const common::IdentityTokenCodec* identity_codec_;
    Clock clock_;
    std::string identity_issuer_;
    /// 指标注册表(#47):progress 判定点直写源站直查量;可空。
    observability::MetricsRegistry* metrics_{nullptr};
};

}  // namespace realm::game::queue
