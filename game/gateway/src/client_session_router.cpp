#include "realmmesh/game/gateway/client_session_router.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace realm::game::gateway {
namespace {

[[nodiscard]] std::size_t protocol_index(
    network::TransportProtocol protocol) noexcept {
    return static_cast<std::size_t>(protocol);
}

}  // namespace

void ClientSessionRouter::register_transport(
    network::IMessageTransport& transport) {
    if (!transports_.emplace(std::string(transport.name()), &transport).second) {
        throw std::invalid_argument("transport is already registered in client router");
    }
}

ClientSessionId ClientSessionRouter::open_tcp_session(
    std::string_view transport_name,
    network::SessionId transport_session_id) {
    auto* transport = find_transport(transport_name);
    if (transport == nullptr ||
        transport->protocol() != network::TransportProtocol::Tcp ||
        transport_session_id == network::invalid_session_id) {
        return invalid_client_session_id;
    }

    ChannelKey key{std::string(transport_name), transport_session_id};
    if (const auto existing = clients_by_channel_.find(key);
        existing != clients_by_channel_.end()) {
        return existing->second;
    }
    if (next_client_session_id_ == invalid_client_session_id) {
        throw std::overflow_error("client session id space exhausted");
    }

    const ClientSessionId id = next_client_session_id_++;
    ClientSession client{.id = id, .channels = {}};
    client.channels[protocol_index(network::TransportProtocol::Tcp)] = {
        .transport_name = std::string(transport_name),
        .protocol = network::TransportProtocol::Tcp,
        .transport_session_id = transport_session_id,
    };
    clients_.emplace(id, std::move(client));
    clients_by_channel_.emplace(std::move(key), id);
    return id;
}

bool ClientSessionRouter::bind_channel(
    ClientSessionId client_session_id,
    std::string_view transport_name,
    network::SessionId transport_session_id) {
    const auto client_iterator = clients_.find(client_session_id);
    auto* transport = find_transport(transport_name);
    if (client_iterator == clients_.end() || transport == nullptr ||
        transport_session_id == network::invalid_session_id) {
        return false;
    }

    ChannelKey key{std::string(transport_name), transport_session_id};
    const auto owner = clients_by_channel_.find(key);
    if (owner != clients_by_channel_.end() && owner->second != client_session_id) {
        return false;
    }

    auto& slot = client_iterator->second.channels[protocol_index(transport->protocol())];
    if (slot.has_value()) {
        if (slot->transport_name == transport_name &&
            slot->transport_session_id == transport_session_id) {
            return true;
        }
        if (transport->protocol() == network::TransportProtocol::Tcp) {
            return false;
        }
        clients_by_channel_.erase({
            slot->transport_name,
            slot->transport_session_id,
        });
    }

    slot = {
        .transport_name = std::string(transport_name),
        .protocol = transport->protocol(),
        .transport_session_id = transport_session_id,
    };
    clients_by_channel_.insert_or_assign(std::move(key), client_session_id);
    return true;
}

void ClientSessionRouter::close_channel(
    std::string_view transport_name,
    network::SessionId transport_session_id) {
    ChannelKey key{std::string(transport_name), transport_session_id};
    const auto owner = clients_by_channel_.find(key);
    if (owner == clients_by_channel_.end()) {
        return;
    }

    const ClientSessionId client_id = owner->second;
    const auto client = clients_.find(client_id);
    if (client == clients_.end()) {
        clients_by_channel_.erase(owner);
        return;
    }

    const auto channel_iterator = std::ranges::find_if(
        client->second.channels,
        [&](const auto& channel) {
            return channel.has_value() &&
                   channel->transport_name == transport_name &&
                   channel->transport_session_id == transport_session_id;
        });
    if (channel_iterator == client->second.channels.end()) {
        clients_by_channel_.erase(owner);
        return;
    }
    if ((*channel_iterator)->protocol == network::TransportProtocol::Tcp) {
        erase_client(client_id);
        return;
    }
    channel_iterator->reset();
    clients_by_channel_.erase(owner);
}

std::optional<ClientSessionId> ClientSessionRouter::find_client(
    std::string_view transport_name,
    network::SessionId transport_session_id) const {
    const auto iterator = clients_by_channel_.find({
        std::string(transport_name),
        transport_session_id,
    });
    if (iterator == clients_by_channel_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

std::optional<ClientChannel> ClientSessionRouter::channel(
    ClientSessionId client_session_id,
    network::TransportProtocol protocol) const {
    const auto iterator = clients_.find(client_session_id);
    if (iterator == clients_.end()) {
        return std::nullopt;
    }
    return iterator->second.channels[protocol_index(protocol)];
}

std::vector<ClientSessionId> ClientSessionRouter::client_ids() const {
    std::vector<ClientSessionId> ids;
    ids.reserve(clients_.size());
    for (const auto& [id, client] : clients_) {
        static_cast<void>(client);
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    return ids;
}

std::size_t ClientSessionRouter::client_count() const noexcept {
    return clients_.size();
}

SendResult ClientSessionRouter::send(
    ClientSessionId client_session_id,
    std::span<const std::byte> payload,
    SendOptions options) {
    const auto client = clients_.find(client_session_id);
    if (client == clients_.end()) {
        return SendResult::ClientNotFound;
    }

    const auto& preferred =
        client->second.channels[protocol_index(options.preferred)];
    bool preferred_failed = false;
    if (preferred.has_value()) {
        auto* transport = find_transport(preferred->transport_name);
        if (transport != nullptr &&
            transport->send(preferred->transport_session_id, payload)) {
            return SendResult::SentPreferred;
        }
        preferred_failed = true;
    }

    if (options.fallback == FallbackPolicy::UseTcp &&
        options.preferred != network::TransportProtocol::Tcp) {
        const auto& tcp = client->second.channels[
            protocol_index(network::TransportProtocol::Tcp)];
        if (tcp.has_value()) {
            auto* transport = find_transport(tcp->transport_name);
            if (transport != nullptr &&
                transport->send(tcp->transport_session_id, payload)) {
                return SendResult::SentViaTcpFallback;
            }
            return SendResult::SendFailed;
        }
    }
    return preferred_failed
               ? SendResult::SendFailed
               : SendResult::ChannelUnavailable;
}

network::IMessageTransport* ClientSessionRouter::find_transport(
    std::string_view name) const {
    const auto iterator = transports_.find(std::string(name));
    return iterator == transports_.end() ? nullptr : iterator->second;
}

void ClientSessionRouter::erase_client(ClientSessionId client_session_id) {
    const auto iterator = clients_.find(client_session_id);
    if (iterator == clients_.end()) {
        return;
    }
    for (const auto& channel : iterator->second.channels) {
        if (channel.has_value()) {
            clients_by_channel_.erase({
                channel->transport_name,
                channel->transport_session_id,
            });
        }
    }
    clients_.erase(iterator);
}

}  // namespace realm::game::gateway
