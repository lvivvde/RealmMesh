#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include "realmmesh/network/codec/length_field_codec.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <vector>

namespace realm::game::gateway {
namespace {

class SocketGuard final {
public:
    explicit SocketGuard(int descriptor) : descriptor_(descriptor) {}
    ~SocketGuard() { ::close(descriptor_); }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    [[nodiscard]] int get() const noexcept { return descriptor_; }

private:
    int descriptor_;
};

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    std::ranges::transform(text, std::back_inserter(result), [](char value) {
        return static_cast<std::byte>(value);
    });
    return result;
}

int connect_to_loopback(std::uint16_t port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        throw std::runtime_error("failed to create runtime test client");
    }

    timeval timeout{};
    timeout.tv_sec = 1;
    if (::setsockopt(
            descriptor, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0) {
        ::close(descriptor);
        throw std::runtime_error("failed to set runtime test timeout");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(
            descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) < 0) {
        ::close(descriptor);
        throw std::runtime_error("failed to connect runtime test client");
    }
    return descriptor;
}

void send_all(int descriptor, std::span<const std::byte> data) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        const auto sent = ::send(
            descriptor,
            data.data() + offset,
            data.size() - offset,
            MSG_NOSIGNAL);
        if (sent <= 0) {
            throw std::runtime_error("failed to send runtime test frame");
        }
        offset += static_cast<std::size_t>(sent);
    }
}

std::vector<std::byte> receive_exactly(int descriptor, std::size_t size) {
    std::vector<std::byte> result(size);
    std::size_t offset = 0;
    while (offset < size) {
        const auto received = ::recv(
            descriptor,
            result.data() + offset,
            result.size() - offset,
            0);
        if (received <= 0) {
            throw std::runtime_error("failed to receive runtime test frame");
        }
        offset += static_cast<std::size_t>(received);
    }
    return result;
}

TEST(GatewayRuntimeTest, MovesMessagesAcrossBoundedQueuesAndFallsBackToTcp) {
    using namespace std::chrono_literals;

    GatewayRuntime runtime(
        {.transports = {{
             .name = "client_tcp",
             .protocol = network::TransportProtocol::Tcp,
             .listen_address = "127.0.0.1",
             .listen_port = 0,
             .max_sessions = 16,
             .max_payload_size = 1024,
         }}},
        {
            .inbound_capacity = 16,
            .outbound_capacity = 16,
            .io_poll_interval = 1ms,
        });
    runtime.start();
    const SocketGuard client(connect_to_loopback(runtime.local_port()));
    const network::LengthFieldCodec codec(1024);
    const auto payload = bytes("frame-message");
    send_all(client.get(), codec.encode(payload));

    std::optional<GatewayEvent> message;
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (std::chrono::steady_clock::now() < deadline && !message.has_value()) {
        auto event = runtime.try_receive();
        if (event.has_value() &&
            event->kind == network::TransportEventKind::MessageReceived) {
            message = std::move(event);
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }

    ASSERT_TRUE(message.has_value());
    ASSERT_TRUE(message->client_session_id.has_value());
    EXPECT_EQ(message->payload, payload);
    EXPECT_EQ(
        runtime.try_send(
            *message->client_session_id,
            message->payload,
            {.preferred = network::TransportProtocol::Udp}),
        QueueResult::Queued);

    EXPECT_EQ(receive_exactly(client.get(), codec.encode(payload).size()),
              codec.encode(payload));
    EXPECT_EQ(runtime.stats().tcp_fallback_deliveries, 1U);
    runtime.stop();
    EXPECT_FALSE(runtime.running());
}

TEST(GatewayRuntimeTest, RejectsCommandsWhileStopped) {
    GatewayRuntime runtime(
        {.transports = {{
             .name = "client_tcp",
             .protocol = network::TransportProtocol::Tcp,
             .listen_address = "127.0.0.1",
             .listen_port = 0,
         }}},
        {.inbound_capacity = 1, .outbound_capacity = 1});

    EXPECT_EQ(runtime.try_send(1, bytes("ignored")), QueueResult::Stopped);
}

}  // namespace
}  // namespace realm::game::gateway
