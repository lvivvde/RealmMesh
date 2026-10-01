#include "realmmesh/game/gateway/gateway_login_config.hpp"

#include "realmmesh/game/common/admission_grant.hpp"

#include <stdexcept>
#include <string>
#include <string_view>

namespace realm::game::gateway {
namespace {

/// 键环内 kid 必须唯一:同一 kid 两把公钥会让验签结果取决于装载顺序。
void validate_key_ring(const std::vector<AdmissionGrantKeySource>& ring) {
    if (ring.empty()) {
        throw std::invalid_argument(
            "gateway admission key ring must not be empty");
    }
    for (std::size_t index = 0; index < ring.size(); ++index) {
        const auto& source = ring.at(index);
        if (source.kid.empty() || source.public_key_environment.empty()) {
            throw std::invalid_argument(
                "gateway admission key ring entries need kid and "
                "public_key_environment");
        }
        for (std::size_t other = index + 1; other < ring.size(); ++other) {
            if (ring.at(other).kid == source.kid) {
                throw std::invalid_argument(
                    "gateway admission key ring has duplicate kid: " +
                    source.kid);
            }
        }
    }
}

}  // namespace

void GatewayLoginConfig::validate() const {
    if (conn_capacity == 0 || fetch_capacity == 0 || fetch_workers == 0) {
        throw std::invalid_argument(
            "gateway login capacities and fetch workers must be positive");
    }
    if (fetch_timeout <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("gateway fetch timeout must be positive");
    }
    if (fetch_retry_base <= std::chrono::milliseconds::zero() ||
        fetch_retry_max == 0 || fetch_retry_max > 10) {
        throw std::invalid_argument(
            "gateway fetch retry base must be positive and retry max within 1..10");
    }
    if (handoff_grace <= std::chrono::milliseconds::zero()) {
        throw std::invalid_argument("gateway handoff grace must be positive");
    }
    if (static_realm.has_value() &&
        (static_realm->address.empty() || static_realm->port == 0)) {
        throw std::invalid_argument(
            "gateway static realm endpoint must be complete");
    }
    if (identity_kid.empty() || identity_issuer.empty() ||
        admission_grant_issuer.empty() || deployment_id.empty() ||
        admission_consumption_prefix.empty() ||
        admission_reservation_ttl <= std::chrono::seconds::zero()) {
        throw std::invalid_argument("gateway admission configuration is incomplete");
    }
    validate_key_ring(admission_grant_keys);
    // 配置可以收紧但不能抬高协议硬上限:窗口大于上限会让 Queue 的合法
    // 签发在网关侧被误判为过期。
    if (admission_grant_window <= std::chrono::seconds::zero() ||
        admission_grant_window > common::admission_grant_max_window) {
        throw std::invalid_argument(
            "gateway admission grant window must be positive and within the "
            "protocol maximum");
    }
    credential_ingress.validate();
}

}  // namespace realm::game::gateway
