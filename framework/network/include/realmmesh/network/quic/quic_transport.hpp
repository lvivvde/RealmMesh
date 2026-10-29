#pragma once

#include "realmmesh/network/transport/transport_config.hpp"

#include <memory>

namespace realm::observability {
class Logger;
}

namespace realm::network {

/// MsQuic 回调线程与业务线程之间的消息传输适配器。回调只写入内部有界事件队列，
/// 业务侧通过 poll_once() 取走事件；队列过载时连接会被主动关闭。
class QuicTransport final : public IMessageTransport {
public:
    explicit QuicTransport(
        TransportConfig config, observability::Logger* logger = nullptr);
    ~QuicTransport() override;

    QuicTransport(const QuicTransport&) = delete;
    QuicTransport& operator=(const QuicTransport&) = delete;

    [[nodiscard]] std::string_view name() const noexcept override;
    [[nodiscard]] TransportProtocol protocol() const noexcept override;
    [[nodiscard]] TransportEndpoint local_endpoint() const override;
    [[nodiscard]] std::size_t session_count() const noexcept override;
    [[nodiscard]] std::vector<TransportEvent> poll_once(
        std::chrono::milliseconds timeout) override;
    [[nodiscard]] bool send(
        SessionId session_id, std::span<const std::byte> payload) override;
    [[nodiscard]] bool close(SessionId session_id) override;
    [[nodiscard]] bool reload_credentials() override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::network
