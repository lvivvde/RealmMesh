#pragma once

namespace realm::network {

// Which Transport protocol families this build can actually serve. The
// default mirrors the CMake-selected platform capabilities; the parameter on
// TransportFactory::create_enabled lets tests exercise the reduced set that
// platforms without MsQuic run with (ADR-0002).
struct TransportCapabilities {
    bool quic{false};
    bool tls_tcp{true};

    bool operator==(const TransportCapabilities&) const = default;
};

[[nodiscard]] TransportCapabilities platform_transport_capabilities() noexcept;

}  // namespace realm::network
