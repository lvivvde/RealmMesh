#include "realmmesh/network/quic/quic_transport_factory.hpp"

#include "realmmesh/network/quic/quic_transport.hpp"

#include <utility>

namespace realm::network {

std::unique_ptr<IMessageTransport> make_quic_transport(
    TransportConfig config, observability::Logger* logger) {
    return std::make_unique<QuicTransport>(std::move(config), logger);
}

}  // namespace realm::network
