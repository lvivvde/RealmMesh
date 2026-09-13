#include "realmmesh/network/client/tls_tcp_client_dialer.hpp"

#include <memory>
#include <string>
#include <utility>

namespace realm::network::client {
namespace {

[[nodiscard]] ConnectFailure classify(const TlsClientStream::DialResult& result,
                                      bool verify_peer) {
    switch (result.failure) {
        case TlsDialFailure::None:
            return ConnectFailure::ProtocolError;
        case TlsDialFailure::Cancelled:
            return ConnectFailure::Cancelled;
        case TlsDialFailure::Resolve:
        case TlsDialFailure::Socket:
        case TlsDialFailure::Configure:
        case TlsDialFailure::Connect:
        case TlsDialFailure::ConnectTimeout:
            return ConnectFailure::NetworkUnreachable;
        case TlsDialFailure::SslSetup:
            return ConnectFailure::ProtocolError;
        case TlsDialFailure::Handshake:
            if (verify_peer && result.verify_result != 0) {
                return ConnectFailure::CertificateRejected;
            }
            return ConnectFailure::ProtocolError;
    }
    return ConnectFailure::ProtocolError;
}

}  // namespace

TlsTcpClientDialer::TlsTcpClientDialer(std::string alpn, bool verify_peer) {
    options_.alpn = std::move(alpn);
    options_.verify_peer = verify_peer;
}

ConnectAttempt TlsTcpClientDialer::connect(
    const EndpointCandidate& endpoint,
    std::chrono::milliseconds handshake_timeout,
    std::stop_token stop_token) {
    if (endpoint.protocol == TransportProtocol::Quic) {
        return ConnectFailure::Unsupported;
    }

    const auto deadline = StreamClock::now() + handshake_timeout;
    auto result = TlsClientStream::dial(
        endpoint.host, endpoint.port, options_, deadline, stop_token);
    if (!result.ok()) {
        return classify(result, options_.verify_peer);
    }
    // 服务端必须在我们的列表里选一个;没选或选了别的即为协商失败。
    const bool expects_alpn = !options_.alpn.empty();
    const std::string expected = options_.alpn;
    const std::string& negotiated = result.stream->negotiated_alpn();
    if (expects_alpn && negotiated != expected) {
        return ConnectFailure::AlpnRejected;
    }
    return ConnectAttempt{std::shared_ptr<ISecureConnection>{
        std::move(result.stream)}};
}

}  // namespace realm::network::client
