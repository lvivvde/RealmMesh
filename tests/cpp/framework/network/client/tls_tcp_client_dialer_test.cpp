#include "realmmesh/network/client/tls_tcp_client_dialer.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>

namespace realm::network::client {
namespace {

using std::chrono::milliseconds;

/// QUIC 候选在本仓一律 Unsupported(ADR-0002:没有 QUIC 客户端实现),
/// 且 Unsupported 属于可降级失败——竞速据此立刻转 TLS/TCP,不必等
/// 350ms 起跑延迟。
TEST(TlsTcpClientDialerTest, QuicCandidateIsUnsupportedAndFallbackPermitted) {
    TlsTcpClientDialer dialer("realmmesh-edge/1", /*verify_peer=*/false);
    const auto attempt = dialer.connect(
        EndpointCandidate{.protocol = TransportProtocol::Quic,
                          .host = "127.0.0.1",
                          .port = 1,
                          .priority = 0},
        milliseconds{1000}, std::stop_token{});

    const auto* failure = std::get_if<ConnectFailure>(&attempt);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(*failure, ConnectFailure::Unsupported);
    EXPECT_TRUE(permits_transport_fallback(*failure));
}

/// TLS/TCP 候选连不上(空闲端口 → 拒绝):归 NetworkUnreachable,同样
/// 属可降级失败,竞速不会把它当成终局。
TEST(TlsTcpClientDialerTest, RefusedTlsTcpDialIsNetworkUnreachable) {
    std::uint16_t idle_port = 0;
    {
        const TcpListener listener("127.0.0.1", 0);
        idle_port = listener.local_port();
    }
    ASSERT_NE(idle_port, 0);

    TlsTcpClientDialer dialer("realmmesh-edge/1", /*verify_peer=*/false);
    const auto attempt = dialer.connect(
        EndpointCandidate{.protocol = TransportProtocol::TlsTcp,
                          .host = "127.0.0.1",
                          .port = idle_port,
                          .priority = 0},
        milliseconds{2000}, std::stop_token{});

    const auto* failure = std::get_if<ConnectFailure>(&attempt);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(*failure, ConnectFailure::NetworkUnreachable);
    EXPECT_TRUE(permits_transport_fallback(*failure));
}

}  // namespace
}  // namespace realm::network::client
