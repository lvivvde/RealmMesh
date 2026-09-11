#include "realmmesh/network/transport/transport_capabilities.hpp"

namespace realm::network {

TransportCapabilities platform_transport_capabilities() noexcept {
    return TransportCapabilities{
#if defined(REALM_NETWORK_HAS_QUIC)
        .quic = true,
#else
        .quic = false,
#endif
        .tls_tcp = true,
    };
}

}  // namespace realm::network
