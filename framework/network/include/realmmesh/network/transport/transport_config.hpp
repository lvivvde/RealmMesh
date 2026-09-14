#pragma once

#include "realmmesh/network/transport/message_transport.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace realm::network {

/// edge 线的 ALPN 名:网关段与 Realm 段共用同一条线(ADR-0008),故服务端
/// 默认值与客户端两段拨号器同源一处,避免两段各自漂移。
inline constexpr std::string_view kEdgeAlpn = "realmmesh-edge/1";

struct TransportConfig {
    struct TlsServerIdentity {
        std::string certificate_chain_file;
        std::string private_key_file;
        std::string alpn{kEdgeAlpn};
    };

    std::string name;
    TransportProtocol protocol{TransportProtocol::TlsTcp};
    bool enabled{true};
    std::string listen_address{"0.0.0.0"};
    std::uint16_t listen_port{0};
    std::size_t max_sessions{1024};
    std::size_t max_payload_size{64 * 1024};
    std::size_t max_pending_output_bytes{4 * 1024 * 1024};
    std::optional<TlsServerIdentity> tls;
    std::chrono::milliseconds handshake_timeout{3'000};
    std::chrono::milliseconds idle_timeout{30'000};
};

}  // namespace realm::network
