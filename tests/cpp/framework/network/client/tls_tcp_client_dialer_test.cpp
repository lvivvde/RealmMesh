#include "realmmesh/network/client/tls_tcp_client_dialer.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <stop_token>

namespace realm::network::client {
namespace {

using std::chrono::milliseconds;

class RefusedTcpPort final {
public:
    RefusedTcpPort() : descriptor_(::socket(AF_INET, SOCK_STREAM, 0)) {
        if (descriptor_ < 0) throw std::runtime_error("socket failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = 0;
        if (::bind(descriptor_, reinterpret_cast<const sockaddr*>(&address),
                   sizeof(address)) != 0) {
            ::close(descriptor_);
            throw std::runtime_error("bind failed");
        }
        socklen_t size = sizeof(address);
        if (::getsockname(descriptor_, reinterpret_cast<sockaddr*>(&address),
                          &size) != 0) {
            ::close(descriptor_);
            throw std::runtime_error("getsockname failed");
        }
        port_ = ntohs(address.sin_port);
    }

    ~RefusedTcpPort() { ::close(descriptor_); }
    RefusedTcpPort(const RefusedTcpPort&) = delete;
    RefusedTcpPort& operator=(const RefusedTcpPort&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    int descriptor_;
    std::uint16_t port_{0};
};

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
    const RefusedTcpPort refused;
    ASSERT_NE(refused.port(), 0);

    TlsTcpClientDialer dialer("realmmesh-edge/1", /*verify_peer=*/false);
    const auto attempt = dialer.connect(
        EndpointCandidate{.protocol = TransportProtocol::TlsTcp,
                          .host = "127.0.0.1",
                          .port = refused.port(),
                          .priority = 0},
        milliseconds{2000}, std::stop_token{});

    const auto* failure = std::get_if<ConnectFailure>(&attempt);
    ASSERT_NE(failure, nullptr);
    EXPECT_EQ(*failure, ConnectFailure::NetworkUnreachable);
    EXPECT_TRUE(permits_transport_fallback(*failure));
}

}  // namespace
}  // namespace realm::network::client
