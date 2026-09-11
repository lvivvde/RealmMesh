#include "realmmesh/network/quic/quic_transport_factory.hpp"

#include <stdexcept>

namespace realm::network {

std::unique_ptr<IMessageTransport> make_quic_transport(
    TransportConfig, observability::Logger*) {
    throw std::logic_error(
        "QUIC capability was reported but MsQuic was not available at build "
        "time");
}

}  // namespace realm::network
