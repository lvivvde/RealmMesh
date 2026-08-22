#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace realm::network {

enum class TransportProtocol : std::uint8_t {
    Quic = 1,
    TlsTcp = 2,
};

[[nodiscard]] constexpr std::string_view to_string(
    TransportProtocol protocol) noexcept {
    switch (protocol) {
    case TransportProtocol::Quic:
        return "quic";
    case TransportProtocol::TlsTcp:
        return "tls_tcp";
    }
    return "unknown";
}

struct TransportEndpoint {
    std::string name;
    TransportProtocol protocol{TransportProtocol::TlsTcp};
    std::string address;
    std::uint16_t port{0};

    bool operator==(const TransportEndpoint&) const = default;
};

using SessionId = std::uint64_t;
inline constexpr SessionId invalid_session_id = 0;

enum class TransportEventKind : std::uint8_t {
    SessionOpened,
    MessageReceived,
    SessionClosed,
    PeerAddressChanged,
};

struct TransportEvent {
    TransportEventKind kind;
    SessionId session_id{invalid_session_id};
    std::vector<std::byte> payload;
};

class IMessageTransport {
public:
    virtual ~IMessageTransport() = default;

    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual TransportProtocol protocol() const noexcept = 0;
    [[nodiscard]] virtual TransportEndpoint local_endpoint() const = 0;
    [[nodiscard]] virtual std::size_t session_count() const noexcept = 0;

    [[nodiscard]] virtual std::vector<TransportEvent> poll_once(
        std::chrono::milliseconds timeout) = 0;
    [[nodiscard]] virtual bool send(
        SessionId session_id,
        std::span<const std::byte> payload) = 0;
    [[nodiscard]] virtual bool close(SessionId session_id) = 0;
    [[nodiscard]] virtual bool reload_credentials() = 0;
};

}  // namespace realm::network
