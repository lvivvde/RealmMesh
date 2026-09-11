#pragma once

#include "realmmesh/network/transport/transport_capabilities.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

#include <memory>
#include <span>
#include <vector>

namespace realm::observability {
class Logger;
}

namespace realm::network {

class TransportFactory final {
public:
    [[nodiscard]] static std::vector<std::unique_ptr<IMessageTransport>>
    create_enabled(
        std::span<const TransportConfig> configs,
        observability::Logger* logger = nullptr,
        TransportCapabilities capabilities = platform_transport_capabilities());
};

}  // namespace realm::network
