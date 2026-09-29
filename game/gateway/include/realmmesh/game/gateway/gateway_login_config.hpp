#pragma once

#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_ingress.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realm::game::gateway {

struct RealmEndpoint {
    std::string address;
    std::uint16_t port{0};
    auto operator<=>(const RealmEndpoint&) const = default;
};

/// Admission Grant 验证键环的一个条目(kid + 公钥所在的环境变量名)。
/// 轮换重叠期把退休 kid 一并列在环里,直到该凭据最大寿命 + 时钟容差过去;
/// 私钥只允许存在于 Queue Scheduler,验证侧永不装载。
struct AdmissionGrantKeySource final {
    std::string kid;
    std::string public_key_environment;
};

struct GatewayLoginConfig {
    std::uint64_t conn_capacity{0};
    std::uint64_t fetch_capacity{1'000};
    std::chrono::milliseconds fetch_retry_base{2'000};
    unsigned fetch_retry_max{3};
    std::chrono::milliseconds handoff_grace{5'000};
    std::optional<RealmEndpoint> static_realm;
    GatewayCredentialIngressConfig credential_ingress;
    /// Admission Grant 只以 kid 索引的公钥环验证。
    std::string identity_kid{"login-verify-v1"};
    std::string identity_issuer{"realmmesh/login-verify"};
    std::vector<AdmissionGrantKeySource> admission_grant_keys{
        {"admission-grant-v1", "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY"}};
    std::string admission_grant_issuer{"realmmesh/queue"};
    /// 必须与 Queue 的签发窗口一致;协议硬上限见 admission_grant_max_window。
    std::chrono::seconds admission_grant_window{300};
    std::string deployment_id{"development"};
    std::string admission_consumption_prefix{
        "/realmmesh/admission/consumption"};
    std::chrono::seconds admission_reservation_ttl{10};

    void validate() const;
};

}  // namespace realm::game::gateway
