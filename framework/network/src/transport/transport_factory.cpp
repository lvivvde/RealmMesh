#include "realmmesh/network/transport/transport_factory.hpp"

#include "realmmesh/network/tcp/tcp_transport.hpp"
#include "realmmesh/network/udp/udp_transport.hpp"
#include "realmmesh/network/kcp/kcp_transport.hpp"

#include <stdexcept>
#include <string>
#include <unordered_set>

namespace realm::network {
namespace {

void validate(const TransportConfig& config) {
    if (config.name.empty()) {
        throw std::invalid_argument("transport name cannot be empty");
    }
    if (config.listen_address.empty()) {
        throw std::invalid_argument("transport listen address cannot be empty");
    }
    if (config.max_sessions == 0) {
        throw std::invalid_argument("transport max sessions must be positive");
    }
    if (config.max_payload_size == 0) {
        throw std::invalid_argument("transport max payload size must be positive");
    }
    if (config.protocol == TransportProtocol::Tcp &&
        config.max_pending_output_bytes == 0) {
        throw std::invalid_argument("TCP output high watermark must be positive");
    }
}

}  // namespace

std::vector<std::unique_ptr<IMessageTransport>>
TransportFactory::create_enabled(std::span<const TransportConfig> configs) {
    std::vector<std::unique_ptr<IMessageTransport>> transports;
    std::unordered_set<std::string> names;

    for (const auto& config : configs) {
        if (!config.enabled) {
            continue;
        }
        validate(config);
        if (!names.emplace(config.name).second) {
            throw std::invalid_argument("enabled transport names must be unique");
        }

        switch (config.protocol) {
        case TransportProtocol::Tcp:
            transports.push_back(std::make_unique<TcpTransport>(config));
            break;
        case TransportProtocol::Udp:
            transports.push_back(std::make_unique<UdpTransport>(config));
            break;
        case TransportProtocol::Kcp:
            transports.push_back(std::make_unique<KcpTransport>(config));
            break;
        }
    }
    return transports;
}

}  // namespace realm::network
