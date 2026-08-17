#include "realmmesh/network/tcp/tcp_transport.hpp"

#include <utility>

namespace realm::network {

TcpTransport::TcpTransport(TransportConfig config)
    : config_(std::move(config)),
      listener_(config_.listen_address, config_.listen_port) {
    event_loop_.add(listener_.native_handle(), EventInterest::Read);
}

std::string_view TcpTransport::name() const noexcept {
    return config_.name;
}

TransportProtocol TcpTransport::protocol() const noexcept {
    return TransportProtocol::Tcp;
}

TransportEndpoint TcpTransport::local_endpoint() const {
    return {
        .name = config_.name,
        .protocol = TransportProtocol::Tcp,
        .address = config_.listen_address,
        .port = listener_.local_port(),
    };
}

std::size_t TcpTransport::session_count() const noexcept {
    return connections_.size();
}

std::vector<TransportEvent> TcpTransport::poll_once(
    std::chrono::milliseconds timeout) {
    const auto ready_events = event_loop_.wait(timeout);
    std::vector<TransportEvent> events;
    std::vector<int> connections_to_close;

    for (const auto& ready : ready_events) {
        if (ready.descriptor == listener_.native_handle()) {
            if (ready.readable) {
                accept_connections(events);
            }
            continue;
        }

        auto iterator = connections_.find(ready.descriptor);
        if (iterator == connections_.end()) {
            continue;
        }

        auto& entry = iterator->second;
        bool close_now = ready.error;
        bool received_messages = false;

        if (!close_now && ready.readable) {
            const auto batch = entry.connection.receive_frames();
            if (batch.status == ReceiveStatus::FrameTooLarge) {
                close_now = true;
            } else {
                received_messages = !batch.frames.empty();
                for (auto frame : batch.frames) {
                    events.push_back({
                        .kind = TransportEventKind::MessageReceived,
                        .session_id = entry.session_id,
                        .payload = std::move(frame),
                    });
                }
                entry.close_after_flush =
                    batch.status == ReceiveStatus::PeerClosed;
            }
        } else if (ready.peer_closed) {
            entry.close_after_flush = true;
        }

        if (!close_now &&
            (ready.writable || entry.connection.has_pending_output())) {
            entry.connection.flush_output();
        }

        if (close_now ||
            (entry.close_after_flush &&
             !entry.connection.has_pending_output() &&
             !received_messages)) {
            connections_to_close.push_back(ready.descriptor);
        } else {
            update_interest(entry);
        }
    }

    for (const int descriptor : connections_to_close) {
        close_descriptor(descriptor, &events);
    }
    return events;
}

bool TcpTransport::send(
    SessionId session_id,
    std::span<const std::byte> payload) {
    const auto descriptor_iterator = descriptors_.find(session_id);
    if (descriptor_iterator == descriptors_.end()) {
        return false;
    }
    auto connection_iterator = connections_.find(descriptor_iterator->second);
    if (connection_iterator == connections_.end()) {
        return false;
    }

    auto& entry = connection_iterator->second;
    if (!entry.connection.queue_frame(payload)) {
        return false;
    }
    entry.connection.flush_output();
    update_interest(entry);
    return true;
}

bool TcpTransport::close(SessionId session_id) {
    const auto iterator = descriptors_.find(session_id);
    if (iterator == descriptors_.end()) {
        return false;
    }
    close_descriptor(iterator->second);
    return true;
}

void TcpTransport::accept_connections(std::vector<TransportEvent>& events) {
    while (true) {
        auto socket = listener_.accept();
        if (!socket.has_value()) {
            return;
        }
        if (connections_.size() >= config_.max_sessions) {
            continue;
        }

        const int descriptor = socket->native_handle();
        const SessionId session_id = next_session_id_++;
        event_loop_.add(descriptor, EventInterest::Read);
        connections_.try_emplace(
            descriptor,
            ConnectionEntry{
                session_id,
                TcpConnection(
                    std::move(*socket),
                    config_.max_payload_size,
                    config_.max_pending_output_bytes),
            });
        descriptors_.emplace(session_id, descriptor);
        events.push_back({
            .kind = TransportEventKind::SessionOpened,
            .session_id = session_id,
            .payload = {},
        });
    }
}

void TcpTransport::close_descriptor(
    int descriptor,
    std::vector<TransportEvent>* events) {
    const auto iterator = connections_.find(descriptor);
    if (iterator == connections_.end()) {
        return;
    }

    const SessionId session_id = iterator->second.session_id;
    event_loop_.remove(descriptor);
    descriptors_.erase(session_id);
    connections_.erase(iterator);
    if (events != nullptr) {
        events->push_back({
            .kind = TransportEventKind::SessionClosed,
            .session_id = session_id,
            .payload = {},
        });
    }
}

void TcpTransport::update_interest(ConnectionEntry& entry) {
    auto interest = entry.close_after_flush
                        ? EventInterest::Write
                        : EventInterest::Read;
    if (!entry.close_after_flush && entry.connection.has_pending_output()) {
        interest = interest | EventInterest::Write;
    }
    event_loop_.modify(entry.connection.native_handle(), interest);
}

}  // namespace realm::network
