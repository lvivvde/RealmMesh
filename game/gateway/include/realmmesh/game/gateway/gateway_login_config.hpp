#pragma once

#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/gateway/gateway_ingress.hpp"

#include <chrono>
#include <compare>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace realm::game::gateway {

struct RealmEndpoint {
    std::string address;
    std::uint16_t port{0};
    auto operator<=>(const RealmEndpoint&) const = default;
};

struct GatewayLoginConfig {
    std::uint64_t conn_capacity{0};
    std::uint64_t fetch_capacity{1'000};
    std::chrono::milliseconds fetch_retry_base{2'000};
    unsigned fetch_retry_max{3};
    std::chrono::milliseconds handoff_grace{5'000};
    std::optional<RealmEndpoint> static_realm;
    GatewayCredentialIngressConfig credential_ingress;

    void validate() const;
};

struct GatewaySigningMaterial {
    common::Ed25519Seed identity_seed;
    std::string identity_kid;
    common::Ed25519Seed queue_seed;
    std::string queue_kid;
    common::SessionTicketKey enter_realm_key;
    std::string identity_issuer;

    [[nodiscard]] static GatewaySigningMaterial from_hex(
        std::string_view identity_seed_hex,
        std::string identity_kid,
        std::string_view queue_seed_hex,
        std::string queue_kid,
        std::string_view enter_realm_key_hex,
        std::string identity_issuer);
};

[[nodiscard]] GatewaySigningMaterial load_gateway_signing_material();

}  // namespace realm::game::gateway
