#pragma once

#include "realmmesh/network/client/secure_byte_stream.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace realm::network::client {

struct EndpointCandidate {
    TransportProtocol protocol{TransportProtocol::Quic};
    std::string host;
    std::uint16_t port{0};
    std::uint32_t priority{0};
};

enum class ConnectFailure {
    Unsupported,
    NetworkUnreachable,
    HandshakeTimeout,
    CertificateRejected,
    AlpnRejected,
    AuthenticationRejected,
    ProtocolError,
    Cancelled,
    NoCandidate,
};

/// 只有网络能力类失败允许切换传输；证书、ALPN、鉴权与协议错误必须原样失败，
/// 防止 fallback 掩盖安全配置或对端身份问题。
[[nodiscard]] constexpr bool permits_transport_fallback(
    ConnectFailure failure) noexcept {
    return failure == ConnectFailure::Unsupported ||
           failure == ConnectFailure::NetworkUnreachable ||
           failure == ConnectFailure::HandshakeTimeout;
}

using ConnectAttempt =
    std::variant<std::shared_ptr<ISecureConnection>, ConnectFailure>;

class ITransportDialer {
public:
    virtual ~ITransportDialer() = default;
    [[nodiscard]] virtual ConnectAttempt connect(
        const EndpointCandidate& endpoint,
        std::chrono::milliseconds handshake_timeout,
        std::stop_token stop_token) = 0;
};

struct ConnectorOptions {
    std::chrono::milliseconds tls_tcp_delay{350};
    std::chrono::milliseconds handshake_timeout{3'000};
    std::chrono::milliseconds round_timeout{5'000};
    std::chrono::milliseconds quic_negative_cache_ttl{300'000};
};

/// QUIC 先发、TLS/TCP 延迟参与的 staged race。胜出后只返回一个安全连接；
/// dialer 必须响应 stop_token，以便及时取消落败和整轮超时的尝试。
class PreferredTransportConnector final {
public:
    explicit PreferredTransportConnector(
        ITransportDialer& dialer,
        ConnectorOptions options = {});

    [[nodiscard]] ConnectAttempt connect(
        std::span<const EndpointCandidate> candidates,
        std::string_view network_id);

    void network_changed();

private:
    struct NegativeCacheEntry {
        std::string network_id;
        std::chrono::steady_clock::time_point expires_at;
    };

    [[nodiscard]] bool quic_is_suppressed(std::string_view network_id) const;
    void remember_quic_failure(
        std::string_view network_id,
        ConnectFailure failure);

    ITransportDialer& dialer_;
    ConnectorOptions options_;
    std::optional<NegativeCacheEntry> quic_negative_cache_;
};

}  // namespace realm::network::client
