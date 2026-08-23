#include "realmmesh/network/tls/tls_tcp_transport.hpp"

#include "realmmesh/observability/logger.hpp"

#include <stdexcept>
#include <utility>

namespace realm::network {

TlsTcpTransport::TlsTcpTransport(
    TransportConfig config, observability::Logger* logger)
    : config_(std::move(config)),
      logger_(logger),
      tls_context_(std::make_unique<TlsServerContext>(config_.tls.value())),
      listener_(config_.listen_address, config_.listen_port) {
    event_loop_.add(listener_.native_handle(), EventInterest::Read);
}

std::string_view TlsTcpTransport::name() const noexcept { return config_.name; }
TransportProtocol TlsTcpTransport::protocol() const noexcept {
    return TransportProtocol::TlsTcp;
}
TransportEndpoint TlsTcpTransport::local_endpoint() const {
    return {
        .name = config_.name,
        .protocol = TransportProtocol::TlsTcp,
        .address = config_.listen_address,
        .port = listener_.local_port(),
    };
}
std::size_t TlsTcpTransport::session_count() const noexcept {
    return connections_.size();
}

std::vector<TransportEvent> TlsTcpTransport::poll_once(
    std::chrono::milliseconds timeout) {
    const auto ready_events = event_loop_.wait(timeout);
    std::vector<TransportEvent> events;
    std::vector<PendingClose> connections_to_close;
    for (const auto& ready : ready_events) {
        if (ready.descriptor == listener_.native_handle()) {
            if (ready.readable) {
                accept_connections();
            }
            continue;
        }
        const auto iterator = connections_.find(ready.descriptor);
        if (iterator != connections_.end()) {
            service_connection(
                iterator->second, ready, events, connections_to_close);
        }
    }

    const auto now = std::chrono::steady_clock::now();
    for (const auto& [descriptor, entry] : connections_) {
        if (!entry.handshake_complete &&
            now - entry.accepted_at >= config_.handshake_timeout) {
            connections_to_close.emplace_back(
                descriptor, "tls_handshake_timeout");
        } else if (
            entry.handshake_complete &&
            now - entry.last_activity >= config_.idle_timeout) {
            connections_to_close.emplace_back(descriptor, "idle_timeout");
        }
    }
    for (const auto& [descriptor, reason] : connections_to_close) {
        close_descriptor(descriptor, &events, reason);
    }
    return events;
}

bool TlsTcpTransport::send(
    SessionId session_id, std::span<const std::byte> payload) {
    const auto descriptor = descriptors_.find(session_id);
    if (descriptor == descriptors_.end()) {
        return false;
    }
    auto iterator = connections_.find(descriptor->second);
    if (iterator == connections_.end() ||
        !iterator->second.handshake_complete) {
        return false;
    }
    auto& entry = iterator->second;
    if (!entry.connection.queue_frame(payload)) {
        return false;
    }
    entry.io_need = entry.connection.flush_output();
    entry.last_activity = std::chrono::steady_clock::now();
    if (entry.io_need == TlsIoState::Failed ||
        entry.io_need == TlsIoState::Closed) {
        close_descriptor(descriptor->second, nullptr, "send_failed");
        return false;
    }
    update_interest(entry);
    return true;
}

bool TlsTcpTransport::close(SessionId session_id) {
    const auto iterator = descriptors_.find(session_id);
    if (iterator == descriptors_.end()) {
        return false;
    }
    close_descriptor(iterator->second);
    return true;
}

bool TlsTcpTransport::reload_credentials() {
    try {
        auto replacement =
            std::make_unique<TlsServerContext>(config_.tls.value());
        tls_context_.swap(replacement);
        return true;
    } catch (...) {
        return false;
    }
}

void TlsTcpTransport::accept_connections() {
    while (true) {
        auto socket = listener_.accept();
        if (!socket) {
            return;
        }
        if (connections_.size() >= config_.max_sessions) {
            continue;
        }
        const int descriptor = socket->native_handle();
        const SessionId session_id = next_session_id_++;
        event_loop_.add(descriptor, EventInterest::Read);
        const auto accepted_at = std::chrono::steady_clock::now();
        const auto peer = socket->peer_endpoint();
        connections_.try_emplace(
            descriptor,
            ConnectionEntry{
                .session_id = session_id,
                .connection = TlsConnection(
                    std::move(*socket),
                    tls_context_->native_handle(),
                    config_.max_payload_size,
                    config_.max_pending_output_bytes),
                .accepted_at = accepted_at,
                .last_activity = accepted_at,
            });
        descriptors_.emplace(session_id, descriptor);
        if (logger_ != nullptr) {
            static_cast<void>(logger_->info(
                "connection_accepted",
                "tls transport accepted incoming connection",
                {observability::field("transport_name", config_.name),
                 observability::field("protocol", "tls_tcp"),
                 observability::field("peer_address", peer.host),
                 observability::field("peer_port", peer.port),
                 observability::field("session_id", session_id)}));
        }
    }
}

void TlsTcpTransport::service_connection(
    ConnectionEntry& entry,
    const ReadyEvent& ready,
    std::vector<TransportEvent>& events,
    std::vector<PendingClose>& connections_to_close) {
    bool close_now = ready.error;
    bool received_messages = false;
    if (ready.error) {
        entry.close_reason = "transport_error";
    }

    if (!close_now && !entry.handshake_complete) {
        entry.io_need = entry.connection.accept_handshake();
        if (entry.io_need == TlsIoState::Ready) {
            entry.handshake_complete = true;
            entry.last_activity = std::chrono::steady_clock::now();
            events.push_back({
                .kind = TransportEventKind::SessionOpened,
                .session_id = entry.session_id,
                .payload = {},
            });
            if (logger_ != nullptr) {
                static_cast<void>(logger_->info(
                    "tls_handshake_completed",
                    "tls transport completed handshake",
                    {observability::field("transport_name", config_.name),
                     observability::field("protocol", "tls_tcp"),
                     observability::field("session_id", entry.session_id),
                     observability::field("alpn", config_.tls->alpn)}));
            }
        } else if (
            entry.io_need == TlsIoState::Closed ||
            entry.io_need == TlsIoState::Failed) {
            close_now = true;
            entry.close_reason = entry.io_need == TlsIoState::Closed
                                     ? "peer_closed_during_handshake"
                                     : "tls_handshake_failed";
        }
    }

    if (!close_now && entry.handshake_complete && ready.readable) {
        auto received = entry.connection.receive_frames();
        entry.io_need = received.state;
        if (received.batch.status == ReceiveStatus::FrameTooLarge) {
            close_now = true;
            entry.close_reason = "frame_too_large";
        } else if (received.state == TlsIoState::Failed) {
            close_now = true;
            entry.close_reason = "receive_failed";
        } else {
            received_messages = !received.batch.frames.empty();
            if (received_messages) {
                entry.last_activity = std::chrono::steady_clock::now();
            }
            for (auto& frame : received.batch.frames) {
                events.push_back({
                    .kind = TransportEventKind::MessageReceived,
                    .session_id = entry.session_id,
                    .payload = std::move(frame),
                });
            }
            if (received.batch.status == ReceiveStatus::PeerClosed) {
                entry.close_after_flush = true;
                entry.close_reason = "peer_closed";
            }
        }
    } else if (ready.peer_closed) {
        entry.close_after_flush = true;
        entry.close_reason = "peer_closed";
    }

    if (!close_now && entry.handshake_complete &&
        (ready.writable || entry.connection.has_pending_output())) {
        entry.io_need = entry.connection.flush_output();
        if (entry.io_need == TlsIoState::Closed ||
            entry.io_need == TlsIoState::Failed) {
            close_now = true;
            entry.close_reason = "flush_failed";
        }
    }

    if (close_now ||
        (entry.close_after_flush && !entry.connection.has_pending_output() &&
         !received_messages)) {
        connections_to_close.emplace_back(
            entry.connection.native_handle(), entry.close_reason);
    } else {
        update_interest(entry);
    }
}

void TlsTcpTransport::close_descriptor(
    int descriptor,
    std::vector<TransportEvent>* events,
    std::string_view reason) {
    const auto iterator = connections_.find(descriptor);
    if (iterator == connections_.end()) {
        return;
    }
    const SessionId session_id = iterator->second.session_id;
    const bool was_open = iterator->second.handshake_complete;
    event_loop_.remove(descriptor);
    descriptors_.erase(session_id);
    connections_.erase(iterator);
    if (events != nullptr && was_open) {
        events->push_back({
            .kind = TransportEventKind::SessionClosed,
            .session_id = session_id,
            .payload = {},
        });
    }
    if (logger_ != nullptr) {
        static_cast<void>(logger_->info(
            "connection_closed",
            "tls transport closed session",
            {observability::field("transport_name", config_.name),
             observability::field("protocol", "tls_tcp"),
             observability::field("session_id", session_id),
             observability::field("reason", reason)}));
    }
}

void TlsTcpTransport::update_interest(ConnectionEntry& entry) {
    auto interest = EventInterest::Read;
    if (entry.io_need == TlsIoState::WantWrite ||
        entry.connection.has_pending_output()) {
        interest = interest | EventInterest::Write;
    }
    event_loop_.modify(entry.connection.native_handle(), interest);
}

}  // namespace realm::network
