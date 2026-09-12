#pragma once

#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/queue/queue_core.hpp"
#include "realmmesh/game/common/queue_number.hpp"
#include "realmmesh/network/http/http1_parser.hpp"
#include "realmmesh/network/http/http1_response.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace realm::game::queue {

/// 排队 HTTP 路由(§5.1 行 2/3/4 + /healthz):POST /v1/queue/tickets
/// (Bearer 身份 Token,同 jti 幂等)、GET /v1/queue/progress(CDN 可
/// 缓存)、GET /v1/queue/tickets/me(Bearer 号牌,放行即重签)。
/// 错误模型 {code, message}:1001 身份凭据无效(1xxx 沿用 edge.proto)、
/// 2001 号牌无效/过期、1000 其余请求性错误。
class QueueHandler final {
public:
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    static constexpr int error_invalid_request = 1000;
    static constexpr int error_invalid_credentials = 1001;
    static constexpr int error_invalid_number = 2001;

    QueueHandler(
        QueueCore& core,
        const common::IdentityTokenCodec& identity_codec,
        const common::QueueNumberCodec& number_codec,
        Clock clock,
        std::string_view identity_issuer,
        std::chrono::seconds queued_number_ttl,
        std::chrono::seconds admit_grace);

    [[nodiscard]] network::Http1Response handle(
        const network::Http1Request& request) const;

private:
    QueueCore* core_;
    const common::IdentityTokenCodec* identity_codec_;
    const common::QueueNumberCodec* number_codec_;
    Clock clock_;
    std::string identity_issuer_;
    std::chrono::seconds queued_number_ttl_;
    std::chrono::seconds admit_grace_;
};

}  // namespace realm::game::queue
