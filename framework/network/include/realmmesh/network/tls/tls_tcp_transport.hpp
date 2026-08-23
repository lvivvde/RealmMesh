#pragma once

#include "realmmesh/network/reactor/epoll_event_loop.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/network/tls/tls_connection.hpp"
#include "realmmesh/network/tls/tls_server_context.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

#include <chrono>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace realm::observability {
class Logger;
}

namespace realm::network {

class TlsTcpTransport final : public IMessageTransport {
public:
    explicit TlsTcpTransport(
        TransportConfig config, observability::Logger* logger = nullptr);

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] TransportProtocol protocol() const noexcept override;
    [[nodiscard]] TransportEndpoint local_endpoint() const override;
    [[nodiscard]] std::size_t session_count() const noexcept override;
    [[nodiscard]] std::vector<TransportEvent> poll_once(
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool send(
        SessionId session_id, std::span<const std::byte> payload) override;
    [[nodiscard]] bool close(SessionId session_id) override;
    [[nodiscard]] bool reload_credentials() override;

private:
    struct ConnectionEntry {
        SessionId session_id;
        TlsConnection connection;
        std::chrono::steady_clock::time_point accepted_at;
        std::chrono::steady_clock::time_point last_activity;
        bool handshake_complete{false};
        bool close_after_flush{false};
        std::string_view close_reason{"unknown"};
        TlsIoState io_need{TlsIoState::WantRead};
    };

    using PendingClose = std::pair<int, std::string_view>;

    void accept_connections();
    void service_connection(
        ConnectionEntry& entry,
        const ReadyEvent& ready,
        std::vector<TransportEvent>& events,
        std::vector<PendingClose>& connections_to_close);
    void close_descriptor(
        int descriptor,
        std::vector<TransportEvent>* events = nullptr,
        std::string_view reason = "application_requested");
    void update_interest(ConnectionEntry& entry);

    TransportConfig config_;
    observability::Logger* logger_{nullptr};
    std::unique_ptr<TlsServerContext> tls_context_;
    TcpListener listener_;
    EpollEventLoop event_loop_;
    SessionId next_session_id_{1};
    std::unordered_map<int, ConnectionEntry> connections_;
    std::unordered_map<SessionId, int> descriptors_;
};

}  // namespace realm::network
