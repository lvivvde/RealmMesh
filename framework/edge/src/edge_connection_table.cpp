#include "realmmesh/edge/edge_connection_table.hpp"

#include <stdexcept>
#include <utility>

namespace realm::edge {

void EdgeConnectionTable::register_transport(
    network::IMessageTransport& transport) {
    if (!transports_.emplace(std::string(transport.name()), &transport)
             .second) {
        throw std::invalid_argument(
            "transport is already registered in the edge session table");
    }
}

EdgeConnectionId EdgeConnectionTable::open(
    std::string_view transport_name,
    network::SessionId transport_session_id,
    std::string source) {
    auto* transport = find_transport(transport_name);
    if (transport == nullptr ||
        transport_session_id == network::invalid_session_id || source.empty()) {
        throw std::invalid_argument(
            "cannot open an edge session on an unknown transport");
    }
    if (const auto existing = by_transport_.find(std::string(transport_name));
        existing != by_transport_.end()) {
        if (const auto session = existing->second.find(transport_session_id);
            session != existing->second.end()) {
            return session->second;
        }
    }
    if (next_connection_id_.value == invalid_edge_connection_id.value) {
        throw std::overflow_error("edge session id space exhausted");
    }

    const EdgeConnectionId connection_id = next_connection_id_;
    next_connection_id_ = EdgeConnectionId{next_connection_id_.value + 1};
    connections_.emplace(
        connection_id,
        EdgeConnectionRecord{
            .primary = EdgeConnectionTransport{
                .transport_name = std::string(transport_name),
                .protocol = transport->protocol(),
                .transport_session_id = transport_session_id,
            },
            .source = std::move(source),
            .established = false,
        });
    by_transport_[std::string(transport_name)].emplace(
        transport_session_id, connection_id);
    return connection_id;
}

bool EdgeConnectionTable::update_source(
    EdgeConnectionId connection_id, std::string source) {
    const auto session = connections_.find(connection_id);
    if (session == connections_.end() || source.empty()) return false;
    session->second.source = std::move(source);
    return true;
}

bool EdgeConnectionTable::establish(EdgeConnectionId connection_id) {
    const auto session = connections_.find(connection_id);
    if (session == connections_.end() || session->second.established) {
        return false;
    }
    session->second.established = true;
    return true;
}

std::optional<EdgeConnectionId> EdgeConnectionTable::find(
    std::string_view transport_name,
    network::SessionId transport_session_id) const {
    const auto located = by_transport_.find(std::string(transport_name));
    if (located == by_transport_.end()) {
        return std::nullopt;
    }
    const auto session = located->second.find(transport_session_id);
    if (session == located->second.end()) {
        return std::nullopt;
    }
    return session->second;
}

std::optional<EdgeConnectionRecord> EdgeConnectionTable::record(
    EdgeConnectionId connection_id) const {
    const auto session = connections_.find(connection_id);
    if (session == connections_.end()) {
        return std::nullopt;
    }
    return session->second;
}

std::optional<ClosedEdgeConnection> EdgeConnectionTable::on_transport_closed(
    std::string_view transport_name, network::SessionId transport_session_id) {
    const auto located = by_transport_.find(std::string(transport_name));
    if (located == by_transport_.end()) {
        return std::nullopt;
    }
    const auto connection_id = located->second.find(transport_session_id);
    if (connection_id == located->second.end()) {
        return std::nullopt;
    }
    const auto session = connections_.find(connection_id->second);
    if (session == connections_.end()) {
        return std::nullopt;
    }
    const ClosedEdgeConnection closed{
        session->first, session->second.source, session->second.established};
    erase(session);
    return closed;
}

SendResult EdgeConnectionTable::send(
    EdgeConnectionId connection_id, std::span<const std::byte> payload) {
    const auto session = connections_.find(connection_id);
    if (session == connections_.end()) {
        return SendResult::UnknownSession;
    }
    auto* transport = find_transport(session->second.primary.transport_name);
    if (transport == nullptr) {
        return SendResult::UnknownSession;
    }
    return transport->send(session->second.primary.transport_session_id, payload)
               ? SendResult::Sent
               : SendResult::SendFailed;
}

std::optional<ClosedEdgeConnection> EdgeConnectionTable::close(
    EdgeConnectionId connection_id) {
    const auto session = connections_.find(connection_id);
    if (session == connections_.end()) {
        return std::nullopt;
    }
    const ClosedEdgeConnection closed{
        session->first, session->second.source, session->second.established};
    auto* transport = find_transport(session->second.primary.transport_name);
    if (transport != nullptr) {
        static_cast<void>(
            transport->close(session->second.primary.transport_session_id));
    }
    erase(session);
    return closed;
}

network::IMessageTransport* EdgeConnectionTable::find_transport(
    std::string_view transport_name) const {
    const auto iterator = transports_.find(std::string(transport_name));
    return iterator == transports_.end() ? nullptr : iterator->second;
}

void EdgeConnectionTable::erase(Table::iterator session) {
    const EdgeConnectionTransport primary = session->second.primary;
    connections_.erase(session);
    const auto located = by_transport_.find(primary.transport_name);
    if (located == by_transport_.end()) {
        return;
    }
    located->second.erase(primary.transport_session_id);
    if (located->second.empty()) {
        by_transport_.erase(located);
    }
}

}  // namespace realm::edge
