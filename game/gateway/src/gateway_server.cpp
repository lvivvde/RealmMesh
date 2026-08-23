#include "realmmesh/game/gateway/gateway_server.hpp"

#include "realmmesh/network/transport/transport_factory.hpp"

#include <algorithm>
#include <stdexcept>

namespace realm::game::gateway {

GatewayServer::GatewayServer(GatewayConfig config)
    : transports_(
          network::TransportFactory::create_enabled(config.transports)) {
    if (transports_.empty()) {
        throw std::invalid_argument(
            "gateway must have at least one enabled transport");
    }
    for (const auto& transport : transports_) {
        client_registry_.register_transport(*transport);
    }
}

std::uint16_t GatewayServer::local_port() const noexcept {
    return transports_.empty() ? 0 : transports_.front()->local_endpoint().port;
}

std::optional<network::TransportEndpoint> GatewayServer::local_endpoint(
    std::string_view transport_name) const {
    const auto iterator = std::ranges::find_if(
        transports_, [transport_name](const auto& transport) {
            return transport->name() == transport_name;
        });
    if (iterator == transports_.end()) {
        return std::nullopt;
    }
    return (*iterator)->local_endpoint();
}

std::vector<network::TransportEndpoint> GatewayServer::local_endpoints() const {
    std::vector<network::TransportEndpoint> endpoints;
    endpoints.reserve(transports_.size());
    for (const auto& transport : transports_) {
        endpoints.push_back(transport->local_endpoint());
    }
    return endpoints;
}

std::size_t GatewayServer::connection_count() const noexcept {
    std::size_t count = 0;
    for (const auto& transport : transports_) {
        count += transport->session_count();
    }
    return count;
}

std::size_t GatewayServer::pending_connection_count() const noexcept {
    std::size_t count = 0;
    for (const auto& [transport_name, sessions] : pending_connections_) {
        static_cast<void>(transport_name);
        count += sessions.size();
    }
    return count;
}

std::size_t GatewayServer::client_count() const noexcept {
    return client_registry_.client_count();
}

std::vector<ClientSessionId> GatewayServer::client_ids() const {
    return client_registry_.client_ids();
}

std::optional<ClientSessionId> GatewayServer::find_client(
    std::string_view transport_name,
    network::SessionId transport_session_id) const {
    return client_registry_.find_client(transport_name, transport_session_id);
}

std::optional<ClientSessionId> GatewayServer::promote_connection(
    std::string_view transport_name, network::SessionId transport_session_id) {
    const auto pending = pending_connections_.find(std::string(transport_name));
    if (pending == pending_connections_.end() ||
        !pending->second.contains(transport_session_id)) {
        return std::nullopt;
    }

    const auto client_id =
        client_registry_.open_primary(transport_name, transport_session_id);
    if (client_id == invalid_client_session_id) {
        return std::nullopt;
    }
    pending->second.erase(transport_session_id);
    if (pending->second.empty()) {
        pending_connections_.erase(pending);
    }
    return client_id;
}

SendResult GatewayServer::send(
    ClientSessionId client_session_id, std::span<const std::byte> payload) {
    return client_registry_.send(client_session_id, payload);
}

bool GatewayServer::send_channel(
    std::string_view transport_name,
    network::SessionId transport_session_id,
    std::span<const std::byte> payload) {
    auto* transport = find_transport(transport_name);
    return transport != nullptr &&
           transport->send(transport_session_id, payload);
}

bool GatewayServer::close_channel(
    std::string_view transport_name, network::SessionId transport_session_id) {
    auto* transport = find_transport(transport_name);
    if (transport == nullptr || !transport->close(transport_session_id)) {
        return false;
    }
    static_cast<void>(
        client_registry_.close_primary(transport_name, transport_session_id));
    const auto pending = pending_connections_.find(std::string(transport_name));
    if (pending != pending_connections_.end()) {
        pending->second.erase(transport_session_id);
        if (pending->second.empty()) {
            pending_connections_.erase(pending);
        }
    }
    return true;
}

bool GatewayServer::reload_credentials() {
    return std::ranges::all_of(transports_, [](const auto& transport) {
        return transport->reload_credentials();
    });
}

std::vector<GatewayEvent> GatewayServer::poll_events(
    std::chrono::milliseconds timeout) {
    const auto count =
        static_cast<std::chrono::milliseconds::rep>(transports_.size());
    const auto per_transport_timeout =
        count == 0 ? std::chrono::milliseconds::zero() : timeout / count;
    std::vector<GatewayEvent> gateway_events;

    for (const auto& transport : transports_) {
        const auto events = transport->poll_once(per_transport_timeout);
        for (auto event : events) {
            std::optional<ClientSessionId> client_session_id;
            GatewayEventKind gateway_kind{GatewayEventKind::MessageReceived};
            switch (event.kind) {
            case network::TransportEventKind::SessionOpened:
                gateway_kind = GatewayEventKind::ConnectionOpened;
                pending_connections_[std::string(transport->name())].insert(
                    event.session_id);
                break;
            case network::TransportEventKind::SessionClosed:
                gateway_kind = GatewayEventKind::ConnectionClosed;
                client_session_id = client_registry_.find_client(
                    transport->name(), event.session_id);
                static_cast<void>(client_registry_.close_primary(
                    transport->name(), event.session_id));
                if (const auto pending = pending_connections_.find(
                        std::string(transport->name()));
                    pending != pending_connections_.end()) {
                    pending->second.erase(event.session_id);
                    if (pending->second.empty()) {
                        pending_connections_.erase(pending);
                    }
                }
                break;
            case network::TransportEventKind::MessageReceived:
                gateway_kind = GatewayEventKind::MessageReceived;
                client_session_id = client_registry_.find_client(
                    transport->name(), event.session_id);
                break;
            case network::TransportEventKind::PeerAddressChanged:
                gateway_kind = GatewayEventKind::PeerAddressChanged;
                client_session_id = client_registry_.find_client(
                    transport->name(), event.session_id);
                break;
            }
            gateway_events.push_back({
                .kind = gateway_kind,
                .client_session_id = client_session_id,
                .transport_name = std::string(transport->name()),
                .protocol = transport->protocol(),
                .transport_session_id = event.session_id,
                .payload = std::move(event.payload),
            });
        }
    }
    return gateway_events;
}

void GatewayServer::poll_once(std::chrono::milliseconds timeout) {
    for (auto& event : poll_events(timeout)) {
        if (event.kind != GatewayEventKind::MessageReceived) {
            continue;
        }
        if (event.client_session_id.has_value()) {
            static_cast<void>(
                client_registry_.send(*event.client_session_id, event.payload));
        } else {
            static_cast<void>(send_channel(
                event.transport_name,
                event.transport_session_id,
                event.payload));
        }
    }
}

network::IMessageTransport* GatewayServer::find_transport(
    std::string_view transport_name) const {
    const auto iterator = std::ranges::find_if(
        transports_, [transport_name](const auto& transport) {
            return transport->name() == transport_name;
        });
    return iterator == transports_.end() ? nullptr : iterator->get();
}

}  // namespace realm::game::gateway
