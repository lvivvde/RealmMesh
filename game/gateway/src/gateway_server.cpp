#include "realmmesh/game/gateway/gateway_server.hpp"

#include "realmmesh/network/transport/transport_factory.hpp"

#include <algorithm>
#include <stdexcept>

namespace realm::game::gateway {

GatewayServer::GatewayServer(GatewayConfig config)
    : transports_(network::TransportFactory::create_enabled(config.transports)) {
    if (transports_.empty()) {
        throw std::invalid_argument(
            "gateway must have at least one enabled transport");
    }
    for (const auto& transport : transports_) {
        client_router_.register_transport(*transport);
    }
}

std::uint16_t GatewayServer::local_port() const noexcept {
    return transports_.empty() ? 0 : transports_.front()->local_endpoint().port;
}

std::optional<network::TransportEndpoint> GatewayServer::local_endpoint(
    std::string_view transport_name) const {
    const auto iterator = std::ranges::find_if(
        transports_,
        [transport_name](const auto& transport) {
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

std::size_t GatewayServer::client_count() const noexcept {
    return client_router_.client_count();
}

std::vector<ClientSessionId> GatewayServer::client_ids() const {
    return client_router_.client_ids();
}

std::optional<ClientSessionId> GatewayServer::find_client(
    std::string_view transport_name,
    network::SessionId transport_session_id) const {
    return client_router_.find_client(transport_name, transport_session_id);
}

bool GatewayServer::bind_channel(
    ClientSessionId client_session_id,
    std::string_view transport_name,
    network::SessionId transport_session_id) {
    return client_router_.bind_channel(
        client_session_id, transport_name, transport_session_id);
}

SendResult GatewayServer::send(
    ClientSessionId client_session_id,
    std::span<const std::byte> payload,
    SendOptions options) {
    return client_router_.send(client_session_id, payload, options);
}

bool GatewayServer::send_channel(
    std::string_view transport_name,
    network::SessionId transport_session_id,
    std::span<const std::byte> payload) {
    auto* transport = find_transport(transport_name);
    return transport != nullptr && transport->send(transport_session_id, payload);
}

bool GatewayServer::close_channel(
    std::string_view transport_name,
    network::SessionId transport_session_id) {
    auto* transport = find_transport(transport_name);
    if (transport == nullptr || !transport->close(transport_session_id)) {
        return false;
    }
    client_router_.close_channel(transport_name, transport_session_id);
    return true;
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
            switch (event.kind) {
            case network::TransportEventKind::SessionOpened:
                if (transport->protocol() == network::TransportProtocol::Tcp) {
                    client_session_id = client_router_.open_tcp_session(
                        transport->name(), event.session_id);
                }
                break;
            case network::TransportEventKind::SessionClosed:
                client_session_id = client_router_.find_client(
                    transport->name(), event.session_id);
                client_router_.close_channel(transport->name(), event.session_id);
                break;
            case network::TransportEventKind::MessageReceived:
                client_session_id = client_router_.find_client(
                    transport->name(), event.session_id);
                break;
            }
            gateway_events.push_back({
                .kind = event.kind,
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
        if (event.kind != network::TransportEventKind::MessageReceived) {
            continue;
        }
        if (event.client_session_id.has_value()) {
            static_cast<void>(client_router_.send(
                *event.client_session_id,
                event.payload,
                {.preferred = event.protocol}));
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
        transports_,
        [transport_name](const auto& transport) {
            return transport->name() == transport_name;
        });
    return iterator == transports_.end() ? nullptr : iterator->get();
}

}  // namespace realm::game::gateway
