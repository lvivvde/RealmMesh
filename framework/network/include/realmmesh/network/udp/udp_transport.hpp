#pragma once

#include "realmmesh/network/reactor/epoll_event_loop.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

#include <netinet/in.h>

#include <string>
#include <unordered_map>

namespace realm::network {

class UdpTransport final : public IMessageTransport {
public:
    explicit UdpTransport(TransportConfig config);
    ~UdpTransport() override;

    UdpTransport(const UdpTransport&) = delete;
    UdpTransport& operator=(const UdpTransport&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] TransportProtocol protocol() const noexcept override;
    [[nodiscard]] TransportEndpoint local_endpoint() const override;
    [[nodiscard]] std::size_t session_count() const noexcept override;

    [[nodiscard]] std::vector<TransportEvent> poll_once(
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool send(
        SessionId session_id,
        std::span<const std::byte> payload) override;
    [[nodiscard]] bool close(SessionId session_id) override;

private:
    struct PeerAddress {
        sockaddr_in address{};
    };

    [[nodiscard]] static std::string peer_key(const sockaddr_in& address);
    void close_socket() noexcept;

    TransportConfig config_;
    int descriptor_{-1};
    std::uint16_t local_port_{0};
    EpollEventLoop event_loop_;
    SessionId next_session_id_{1};
    std::unordered_map<std::string, SessionId> sessions_by_peer_;
    std::unordered_map<SessionId, PeerAddress> peers_;
};

}  // namespace realm::network
