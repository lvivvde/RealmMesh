#pragma once

#include "realmmesh/network/reactor/epoll_event_loop.hpp"
#include "realmmesh/network/tcp/tcp_connection.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

#include <unordered_map>

namespace realm::network {

class TcpTransport final : public IMessageTransport {
public:
    explicit TcpTransport(TransportConfig config);

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
    struct ConnectionEntry {
        SessionId session_id;
        TcpConnection connection;
        bool close_after_flush{false};
    };

    void accept_connections(std::vector<TransportEvent>& events);
    void close_descriptor(
        int descriptor,
        std::vector<TransportEvent>* events = nullptr);
    void update_interest(ConnectionEntry& entry);

    TransportConfig config_;
    TcpListener listener_;
    EpollEventLoop event_loop_;
    SessionId next_session_id_{1};
    std::unordered_map<int, ConnectionEntry> connections_;
    std::unordered_map<SessionId, int> descriptors_;
};

}  // namespace realm::network
