#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/gateway/gateway_ingress.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <string>
#include <vector>

namespace realm::game::gateway {
namespace {

using namespace std::chrono_literals;

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

TEST(GatewayCredentialIngressTest, RejectsMalformedBeforeVerification) {
    GatewayCredentialIngress ingress({
        .max_attach_envelope_bytes = 1024,
        .max_identity_token_bytes = 128,
        .max_admission_grant_bytes = 128,
        .max_token_decoded_bytes = 96,
        .source_rate_per_second = 10,
        .source_burst = 10,
        .source_throttle_close_after = 3,
        .session_attach_attempts = 4,
        .concurrent_verifications = 1,
        .max_tracked_sources = 8,
    });
    common::EdgeAttach attach;
    attach.set_identity_token("not-a-jws");
    attach.set_admission_grant("not-a-jws");
    const auto payload = common::encode(attach);
    const auto check = ingress.inspect_attach(
        "192.0.2.10", 1, payload, std::chrono::steady_clock::now());
    EXPECT_EQ(check.status, GatewayIngressStatus::Malformed);
    EXPECT_FALSE(check.attach.has_value());
    EXPECT_EQ(ingress.counters().verification_in_flight, 0U);
}

TEST(GatewayCredentialIngressTest, BoundsEnvelopeBeforeDecode) {
    GatewayCredentialIngress ingress({
        .max_attach_envelope_bytes = 8,
        .max_identity_token_bytes = 128,
        .max_admission_grant_bytes = 128,
        .max_token_decoded_bytes = 96,
        .source_rate_per_second = 10,
        .source_burst = 10,
        .source_throttle_close_after = 3,
        .session_attach_attempts = 4,
        .concurrent_verifications = 1,
        .max_tracked_sources = 8,
    });
    const std::vector<std::byte> oversized(9);
    const auto check = ingress.inspect_attach(
        "192.0.2.10", 1, oversized, std::chrono::steady_clock::now());
    EXPECT_EQ(check.status, GatewayIngressStatus::Oversized);
}

}  // namespace
}  // namespace realm::game::gateway
