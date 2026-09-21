#include "realmmesh/game/gateway/gateway_login_config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <utility>

namespace realm::game::gateway {
namespace {

[[nodiscard]] std::string_view required_environment(const char* name) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') {
        throw std::runtime_error(std::string(name) + " is not set");
    }
    return value;
}

}  // namespace

void GatewayLoginConfig::validate() const {
    if (conn_capacity == 0 || fetch_capacity == 0) {
        throw std::invalid_argument(
            "gateway login capacities must be positive");
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
    credential_ingress.validate();
}

GatewaySigningMaterial GatewaySigningMaterial::from_hex(
    std::string_view identity_seed_hex,
    std::string identity_kid,
    std::string_view queue_seed_hex,
    std::string queue_kid,
    std::string_view enter_realm_key_hex,
    std::string identity_issuer) {
    if (identity_kid.empty() || queue_kid.empty() || identity_issuer.empty()) {
        throw std::invalid_argument(
            "gateway signing identifiers must not be empty");
    }
    return {
        .identity_seed = common::parse_identity_seed_hex(identity_seed_hex),
        .identity_kid = std::move(identity_kid),
        .queue_seed = common::parse_identity_seed_hex(queue_seed_hex),
        .queue_kid = std::move(queue_kid),
        .enter_realm_key =
            common::parse_ticket_key_hex(enter_realm_key_hex),
        .identity_issuer = std::move(identity_issuer),
    };
}

GatewaySigningMaterial load_gateway_signing_material() {
    return GatewaySigningMaterial::from_hex(
        required_environment("REALMMESH_IDENTITY_KEY_SEED"),
        "login-verify-v1",
        required_environment("REALMMESH_QUEUE_KEY_SEED"),
        "queue-v1",
        required_environment("REALMMESH_SESSION_TICKET_KEY"),
        "realmmesh/login-verify");
}

}  // namespace realm::game::gateway
