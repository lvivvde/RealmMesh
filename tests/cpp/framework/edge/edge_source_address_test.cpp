#include "realmmesh/edge/edge_source_address.hpp"

#include <gtest/gtest.h>

namespace realm::edge {
namespace {

TEST(EdgeSourceNormalizerTest, IgnoresForwardingHeadersFromUntrustedPeer) {
    EdgeSourceNormalizer normalizer({
        .mode = EdgeSourceMode::TrustedXForwardedFor,
        .trusted_proxy_cidrs = {"10.0.0.0/8"},
    });
    const auto normalized = normalizer.normalize({
        .direct_peer = "192.0.2.10",
        .x_forwarded_for = "198.51.100.20",
    });
    EXPECT_EQ(normalized, "192.0.2.10");
}

TEST(EdgeSourceNormalizerTest, UsesFirstUntrustedForwardedHop) {
    EdgeSourceNormalizer normalizer({
        .mode = EdgeSourceMode::TrustedXForwardedFor,
        .trusted_proxy_cidrs = {"10.0.0.0/8"},
    });
    const auto normalized = normalizer.normalize({
        .direct_peer = "10.1.2.3",
        .x_forwarded_for = "198.51.100.20, 10.2.3.4",
    });
    EXPECT_EQ(normalized, "198.51.100.20");
}

}  // namespace
}  // namespace realm::edge
