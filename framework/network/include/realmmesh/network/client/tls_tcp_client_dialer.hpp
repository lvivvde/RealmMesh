#pragma once

// 竞速里的 TLS/TCP 一路(流式载体)。QUIC 候选一律判 Unsupported:
// 本仓没有 QUIC 客户端实现(ADR-0002:QUIC 只在 Linux 服务端可用),
// 而 Unsupported 属于 permits_transport_fallback,竞速据此立刻转
// TLS/TCP —— 「0ms QUIC + 350ms TLS/TCP」在无 QUIC 客户端时退化为
// 「QUIC 即时否 + TLS/TCP 立即起跑」,候选与优先级语义不变。

#include "realmmesh/network/client/preferred_transport_connector.hpp"
#include "realmmesh/network/client/tls_client_stream.hpp"

#include <chrono>
#include <stop_token>
#include <string>

namespace realm::network::client {

class TlsTcpClientDialer final : public ITransportDialer {
public:
    /// alpn 为期望的应用协议名(如 "realmmesh-edge/1")。
    explicit TlsTcpClientDialer(std::string alpn, bool verify_peer = true);

    [[nodiscard]] ConnectAttempt connect(
        const EndpointCandidate& endpoint,
        std::chrono::milliseconds handshake_timeout,
        std::stop_token stop_token) override;

private:
    TlsClientOptions options_;
};

}  // namespace realm::network::client
