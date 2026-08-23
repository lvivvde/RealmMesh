#include "realmmesh/network/transport/transport_factory.hpp"

#include "realmmesh/network/quic/quic_transport.hpp"
#include "realmmesh/network/tls/tls_tcp_transport.hpp"

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
        throw std::invalid_argument(
            "transport max payload size must be positive");
    }
    if (config.max_pending_output_bytes == 0) {
        throw std::invalid_argument(
            "secure transport output high watermark must be positive");
    }
    if ((config.protocol == TransportProtocol::Quic ||
         config.protocol == TransportProtocol::TlsTcp)) {
        if (!config.tls.has_value()) {
            throw std::invalid_argument(
                "secure transport requires a TLS server identity");
        }
        if (config.tls->alpn.empty()) {
            throw std::invalid_argument(
                "secure transport ALPN cannot be empty");
        }
        if (config.handshake_timeout <= std::chrono::milliseconds::zero()) {
            throw std::invalid_argument(
                "secure transport handshake timeout must be positive");
        }
    }
}

}  // namespace

std::vector<std::unique_ptr<IMessageTransport>>
TransportFactory::create_enabled(
    std::span<const TransportConfig> configs, observability::Logger* logger) {
    std::vector<std::unique_ptr<IMessageTransport>> transports;
    std::unordered_set<std::string> names;

    for (const auto& config : configs) {
        if (!config.enabled) {
            continue;
        }
        validate(config);
        if (!names.emplace(config.name).second) {
            throw std::invalid_argument(
                "enabled transport names must be unique");
        }

        switch (config.protocol) {
        case TransportProtocol::Quic:
            transports.push_back(
                std::make_unique<QuicTransport>(config, logger));
            break;
        case TransportProtocol::TlsTcp:
            transports.push_back(
                std::make_unique<TlsTcpTransport>(config, logger));
            break;
        }
    }
    return transports;
}

}  // namespace realm::network
