#include "realmmesh/game/gateway/gateway_source_address.hpp"

#include <gtest/gtest.h>

namespace realm::game::gateway {
namespace {

TEST(GatewaySourceNormalizerTest, IgnoresForwardingHeadersFromUntrustedPeer) {
    GatewaySourceNormalizer normalizer({
        .mode = GatewaySourceMode::TrustedXForwardedFor,
        .trusted_proxy_cidrs = {"10.0.0.0/8"},
    });
    const auto normalized = normalizer.normalize({
        .direct_peer = "192.0.2.10",
        .x_forwarded_for = "198.51.100.20",
    });
    EXPECT_EQ(normalized, "192.0.2.10");
}

TEST(GatewaySourceNormalizerTest, UsesFirstUntrustedForwardedHop) {
    GatewaySourceNormalizer normalizer({
        .mode = GatewaySourceMode::TrustedXForwardedFor,
        .trusted_proxy_cidrs = {"10.0.0.0/8"},
    });
    const auto normalized = normalizer.normalize({
        .direct_peer = "10.1.2.3",
        .x_forwarded_for = "198.51.100.20, 10.2.3.4",
    });
    EXPECT_EQ(normalized, "198.51.100.20");
}

}  // namespace
}  // namespace realm::game::gateway
