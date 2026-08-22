#include "realmmesh/game/gateway/client_session_registry.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace realm::game::gateway {

void ClientSessionRegistry::register_transport(
    network::IMessageTransport& transport) {
    if (!transports_.emplace(std::string(transport.name()), &transport).second) {
        throw std::invalid_argument(
            "transport is already registered in client registry");
    }
}

ClientSessionId ClientSessionRegistry::open_primary(
    std::string_view transport_name,
    network::SessionId transport_session_id) {
    auto* transport = find_transport(transport_name);
    if (transport == nullptr ||
        transport_session_id == network::invalid_session_id) {
        return invalid_client_session_id;
    }

    PrimaryKey key{std::string(transport_name), transport_session_id};
    if (const auto existing = clients_by_primary_.find(key);
        existing != clients_by_primary_.end()) {
        return existing->second;
    }
    if (next_client_session_id_ == invalid_client_session_id) {
        throw std::overflow_error("client session id space exhausted");
    }

    const ClientSessionId id = next_client_session_id_++;
    primaries_.emplace(id, PrimaryTransport{
        .transport_name = std::string(transport_name),
        .protocol = transport->protocol(),
        .transport_session_id = transport_session_id,
    });
    clients_by_primary_.emplace(std::move(key), id);
    return id;
}

std::optional<ClientSessionId> ClientSessionRegistry::close_primary(
    std::string_view transport_name,
    network::SessionId transport_session_id) {
    PrimaryKey key{std::string(transport_name), transport_session_id};
    const auto owner = clients_by_primary_.find(key);
    if (owner == clients_by_primary_.end()) {
        return std::nullopt;
    }

    const ClientSessionId client_id = owner->second;
    clients_by_primary_.erase(owner);
    primaries_.erase(client_id);
    return client_id;
}

std::optional<ClientSessionId> ClientSessionRegistry::find_client(
    std::string_view transport_name,
    network::SessionId transport_session_id) const {
    const auto iterator = clients_by_primary_.find({
        std::string(transport_name),
        transport_session_id,
    });
    if (iterator == clients_by_primary_.end()) {
        return std::nullopt;
    }
    return iterator->second;
}

std::optional<PrimaryTransport> ClientSessionRegistry::primary(
    ClientSessionId client_session_id) const {
    const auto iterator = primaries_.find(client_session_id);
    return iterator == primaries_.end()
               ? std::nullopt
               : std::optional<PrimaryTransport>(iterator->second);
}

std::vector<ClientSessionId> ClientSessionRegistry::client_ids() const {
    std::vector<ClientSessionId> ids;
    ids.reserve(primaries_.size());
    for (const auto& [id, primary_transport] : primaries_) {
        static_cast<void>(primary_transport);
        ids.push_back(id);
    }
    std::ranges::sort(ids);
    return ids;
}

std::size_t ClientSessionRegistry::client_count() const noexcept {
    return primaries_.size();
}

SendResult ClientSessionRegistry::send(
    ClientSessionId client_session_id,
    std::span<const std::byte> payload) {
    const auto primary_transport = primaries_.find(client_session_id);
    if (primary_transport == primaries_.end()) {
        return SendResult::ClientNotFound;
    }

    auto* transport = find_transport(
        primary_transport->second.transport_name);
    if (transport == nullptr ||
        !transport->send(
            primary_transport->second.transport_session_id,
            payload)) {
        return SendResult::SendFailed;
    }
    return SendResult::Sent;
}

network::IMessageTransport* ClientSessionRegistry::find_transport(
    std::string_view name) const {
    const auto iterator = transports_.find(std::string(name));
    return iterator == transports_.end() ? nullptr : iterator->second;
}

}  // namespace realm::game::gateway
