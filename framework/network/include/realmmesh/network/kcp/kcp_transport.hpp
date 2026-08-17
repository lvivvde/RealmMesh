#pragma once

#include "realmmesh/network/transport/transport_config.hpp"

#include <memory>

namespace realm::network {

class KcpTransport final : public IMessageTransport {
public:
    explicit KcpTransport(TransportConfig config);
    ~KcpTransport() override;

    KcpTransport(const KcpTransport&) = delete;
    KcpTransport& operator=(const KcpTransport&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] TransportProtocol protocol() const noexcept override;
    [[nodiscard]] TransportEndpoint local_endpoint() const override;
    [[nodiscard]] std::size_t session_count() const noexcept override;
    [[nodiscard]] std::vector<TransportEvent> poll_once(
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool send(
        SessionId session_id,
        std::span<const std::byte> payload) override;
    [[nodiscard]] bool close(SessionId session_id) override;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

}  // namespace realm::network
