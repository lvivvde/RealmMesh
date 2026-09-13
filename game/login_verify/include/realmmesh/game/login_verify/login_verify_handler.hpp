#pragma once

#include "realmmesh/game/common/account_store.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/network/http/http1_response.hpp"

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

namespace realm::observability {
class MetricsRegistry;
}  // namespace observability

namespace realm::game::login_verify {

/// 身份 Token 的 iss(主规格 §4)与 TTL:值恒定,轮换只作用于 kid。
inline constexpr std::string_view identity_token_issuer =
    "realmmesh/login-verify";
inline constexpr std::chrono::seconds identity_token_ttl{1800};

/// 错误码分段沿用规格 §5.1:1000 通用请求非法(本票裁决的最小延伸),
/// 1001 凭据无效、1002 封禁、1003 白名单外。
inline constexpr int error_invalid_request{1000};
inline constexpr int error_invalid_credentials{1001};
inline constexpr int error_account_banned{1002};
inline constexpr int error_not_whitelisted{1003};

/// 请求处理缝:路由、AccountStore 认定与身份 Token 签发的纯逻辑面,
/// 不含 socket——HttpServer 的协议级回绝(400/413/431/505)不经过这里。
/// 未知账号与口令错误返回同一种 1001,账号存在性不外泄。
class LoginVerifyHandler final {
public:
    /// 可注入时钟;缺省取系统时间。
    using Clock = std::function<std::chrono::system_clock::time_point()>;

    LoginVerifyHandler(
        const common::AccountStore& store,
        const common::IdentityTokenCodec& codec,
        Clock clock,
        std::string_view issuer = identity_token_issuer,
        std::chrono::seconds ttl = identity_token_ttl,
        observability::MetricsRegistry* metrics = nullptr);

    [[nodiscard]] network::Http1Response handle(
        std::string_view method,
        std::string_view target,
        std::string_view body) const;

private:
    const common::AccountStore* store_;
    const common::IdentityTokenCodec* codec_;
    Clock clock_;
    std::string issuer_;
    std::chrono::seconds ttl_;
    /// 指标注册表(#47):验签/签发判定点直写 verify_* 指标;
    /// 可空(既有单测装配不受影响)。
    observability::MetricsRegistry* metrics_{nullptr};
};

}  // namespace realm::game::login_verify
