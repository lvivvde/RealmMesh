#include "realmmesh/game/gateway/gateway_server.hpp"
#include "realmmesh/game/gateway/gateway_config_loader.hpp"

#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/kcp/kcp_security.hpp"

extern "C" {
#include <ikcp.h>
}

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iterator>
#include <memory>
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
    ~SocketGuard() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

    [[nodiscard]] int get() const noexcept {
        return descriptor_;
    }

private:
    int descriptor_;
};

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(result), [](char value) {
        return static_cast<std::byte>(value);
    });
    return result;
}

int connect_to_loopback(std::uint16_t port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        throw std::runtime_error("failed to create gateway test client");
    }

    timeval timeout{};
    timeout.tv_sec = 1;
    if (::setsockopt(
            descriptor,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) < 0) {
        ::close(descriptor);
        throw std::runtime_error("failed to configure gateway test timeout");
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
        throw std::runtime_error("failed to connect gateway test client");
    }

    return descriptor;
}

int create_udp_client() {
    const int descriptor = ::socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        throw std::runtime_error("failed to create UDP gateway test client");
    }

    timeval timeout{};
    timeout.tv_sec = 1;
    if (::setsockopt(
            descriptor,
            SOL_SOCKET,
            SO_RCVTIMEO,
            &timeout,
            sizeof(timeout)) < 0) {
        ::close(descriptor);
        throw std::runtime_error("failed to configure UDP gateway test timeout");
    }
    return descriptor;
}

void send_udp(
    int descriptor,
    std::uint16_t port,
    std::span<const std::byte> payload) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const auto sent = ::sendto(
        descriptor,
        payload.data(),
        payload.size(),
        0,
        reinterpret_cast<const sockaddr*>(&address),
        sizeof(address));
    if (sent < 0 || static_cast<std::size_t>(sent) != payload.size()) {
        throw std::runtime_error("failed to send UDP gateway test datagram");
    }
}

std::vector<std::byte> receive_udp(int descriptor, std::size_t capacity) {
    std::vector<std::byte> received(capacity);
    const auto size = ::recv(descriptor, received.data(), received.size(), 0);
    if (size < 0) {
        throw std::runtime_error("failed to receive UDP gateway echo datagram");
    }
    received.resize(static_cast<std::size_t>(size));
    return received;
}

struct KcpClientContext {
    int descriptor;
    sockaddr_in server{};
    network::KcpClientSecurity* security;
};

int output_kcp_datagram(
    const char* data,
    int size,
    ikcpcb*,
    void* user) {
    auto& context = *static_cast<KcpClientContext*>(user);
    const auto packet = context.security->protect(std::as_bytes(
        std::span(data, static_cast<std::size_t>(size))));
    const auto sent = ::sendto(
        context.descriptor,
        packet.data(),
        packet.size(),
        0,
        reinterpret_cast<const sockaddr*>(&context.server),
        sizeof(context.server));
    return sent >= 0 && static_cast<std::size_t>(sent) == packet.size() ? 0 : -1;
}

IUINT32 kcp_time_ms() {
    return static_cast<IUINT32>(
        std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

void send_all(int descriptor, std::span<const std::byte> data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto result = ::send(
            descriptor,
            data.data() + sent,
            data.size() - sent,
            MSG_NOSIGNAL);
        if (result <= 0) {
            throw std::runtime_error("failed to send gateway test frame");
        }
        sent += static_cast<std::size_t>(result);
    }
}

std::vector<std::byte> receive_exactly(int descriptor, std::size_t byte_count) {
    std::vector<std::byte> received(byte_count);
    std::size_t offset = 0;
    while (offset < byte_count) {
        const auto result = ::recv(
            descriptor,
            received.data() + offset,
            byte_count - offset,
            0);
        if (result <= 0) {
            throw std::runtime_error("failed to receive gateway echo frame");
        }
        offset += static_cast<std::size_t>(result);
    }
    return received;
}

TEST(GatewayServerTest, EchoesALengthPrefixedFrameOverTcp) {
    using namespace std::chrono_literals;

    GatewayServer server({.transports = {{
        .name = "client_tcp",
        .protocol = network::TransportProtocol::Tcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .max_sessions = 16,
        .max_payload_size = 1024,
    }}});
    const SocketGuard client(connect_to_loopback(server.local_port()));
    const network::LengthFieldCodec codec(1024);
    const auto request = codec.encode(bytes("ping"));

    send_all(client.get(), request);
    server.poll_once(500ms);
    server.poll_once(500ms);

    EXPECT_EQ(server.connection_count(), 1U);
    EXPECT_EQ(receive_exactly(client.get(), request.size()), request);
}

TEST(GatewayServerTest, FallsBackToTcpWhenUdpChannelIsNotBound) {
    using namespace std::chrono_literals;

    GatewayServer server({.transports = {{
        .name = "client_tcp",
        .protocol = network::TransportProtocol::Tcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .max_sessions = 16,
        .max_payload_size = 1024,
    }}});
    const SocketGuard client(connect_to_loopback(server.local_port()));
    server.poll_once(500ms);
    ASSERT_EQ(server.client_count(), 1U);
    ASSERT_EQ(server.client_ids().size(), 1U);

    const auto payload = bytes("udp-preferred-position");
    EXPECT_EQ(
        server.send(
            server.client_ids().front(),
            payload,
            {.preferred = network::TransportProtocol::Udp}),
        SendResult::SentViaTcpFallback);

    const network::LengthFieldCodec codec(1024);
    const auto expected = codec.encode(payload);
    EXPECT_EQ(receive_exactly(client.get(), expected.size()), expected);
}

TEST(GatewayServerTest, EchoesADatagramOverUdp) {
    using namespace std::chrono_literals;

    GatewayServer server({.transports = {{
        .name = "client_udp",
        .protocol = network::TransportProtocol::Udp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .max_sessions = 16,
        .max_payload_size = 1024,
    }}});
    const SocketGuard client(create_udp_client());
    const auto request = bytes("position-update");

    send_udp(client.get(), server.local_port(), request);
    server.poll_once(500ms);

    EXPECT_EQ(server.connection_count(), 1U);
    EXPECT_EQ(receive_udp(client.get(), 1024), request);
}

TEST(GatewayServerTest, EchoesAReliableMessageOverKcp) {
    using namespace std::chrono_literals;

    network::KcpSecurityKey ticket_key{};
    ticket_key.front() = std::byte{0x42};
    GatewayServer server({.transports = {{
        .name = "client_kcp",
        .protocol = network::TransportProtocol::Kcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .max_sessions = 16,
        .max_payload_size = 1024,
        .kcp_ticket_key = ticket_key,
        .idle_timeout = 200ms,
    }}});
    const SocketGuard client(create_udp_client());
    const network::KcpTicketCodec ticket_codec(ticket_key);
    network::KcpClientSecurity security(ticket_codec.issue(
        std::chrono::system_clock::now(), 30s));
    KcpClientContext context{
        .descriptor = client.get(),
        .security = &security,
    };
    context.server.sin_family = AF_INET;
    context.server.sin_port = htons(server.local_port());
    context.server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    std::unique_ptr<ikcpcb, decltype(&ikcp_release)> control(
        ikcp_create(42, &context),
        &ikcp_release);
    ASSERT_NE(control, nullptr);
    ikcp_setoutput(control.get(), &output_kcp_datagram);
    ASSERT_EQ(ikcp_nodelay(control.get(), 1, 10, 2, 1), 0);
    const auto request = bytes("reliable-position-update");
    ASSERT_GE(
        ikcp_send(
            control.get(),
            reinterpret_cast<const char*>(request.data()),
            static_cast<int>(request.size())),
        0);
    ikcp_update(control.get(), kcp_time_ms());
    ikcp_flush(control.get());

    const auto receive_message = [&](int descriptor) {
        std::vector<std::byte> response;
        const auto deadline = std::chrono::steady_clock::now() + 1s;
        while (std::chrono::steady_clock::now() < deadline && response.empty()) {
            server.poll_once(10ms);

            char datagram[65536];
            const auto received = ::recv(
                descriptor, datagram, sizeof(datagram), MSG_DONTWAIT);
            if (received > 0) {
                const auto plaintext = security.unprotect(std::as_bytes(
                    std::span(datagram, static_cast<std::size_t>(received))));
                EXPECT_TRUE(plaintext.has_value());
                if (plaintext.has_value()) {
                    EXPECT_GE(
                        ikcp_input(
                            control.get(),
                            reinterpret_cast<const char*>(plaintext->data()),
                            static_cast<long>(plaintext->size())),
                        0);
                }
            }
            ikcp_update(control.get(), kcp_time_ms());

            const int message_size = ikcp_peeksize(control.get());
            if (message_size >= 0) {
                response.resize(static_cast<std::size_t>(message_size));
                EXPECT_EQ(
                    ikcp_recv(
                        control.get(),
                        reinterpret_cast<char*>(response.data()),
                        message_size),
                    message_size);
            }
            std::this_thread::sleep_for(1ms);
        }
        return response;
    };

    const auto response = receive_message(client.get());

    EXPECT_EQ(server.connection_count(), 1U);
    EXPECT_EQ(response, request);

    const SocketGuard migrated_client(create_udp_client());
    context.descriptor = migrated_client.get();
    const auto migrated_request = bytes("after-address-migration");
    ASSERT_GE(
        ikcp_send(
            control.get(),
            reinterpret_cast<const char*>(migrated_request.data()),
            static_cast<int>(migrated_request.size())),
        0);
    ikcp_update(control.get(), kcp_time_ms());
    ikcp_flush(control.get());

    EXPECT_EQ(receive_message(migrated_client.get()), migrated_request);
    EXPECT_EQ(server.connection_count(), 1U);

    server.poll_once(20ms);
    std::this_thread::sleep_for(220ms);
    server.poll_once(0ms);
    EXPECT_EQ(server.connection_count(), 0U);
}

TEST(GatewayServerTest, RejectsExpiredAndTamperedKcpHandshakes) {
    using namespace std::chrono_literals;

    network::KcpSecurityKey ticket_key{};
    ticket_key.front() = std::byte{0x24};
    GatewayServer server({.transports = {{
        .name = "client_kcp",
        .protocol = network::TransportProtocol::Kcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .max_sessions = 16,
        .max_payload_size = 1024,
        .kcp_ticket_key = ticket_key,
    }}});
    const SocketGuard client(create_udp_client());
    const network::KcpTicketCodec ticket_codec(ticket_key);
    std::array<std::byte, 24> fake_kcp_packet{};

    network::KcpClientSecurity expired(ticket_codec.issue(
        std::chrono::system_clock::now() - 60s, 10s));
    const auto expired_packet = expired.protect(fake_kcp_packet);
    send_udp(client.get(), server.local_port(), expired_packet);
    server.poll_once(20ms);
    EXPECT_EQ(server.connection_count(), 0U);

    network::KcpClientSecurity valid(ticket_codec.issue(
        std::chrono::system_clock::now(), 10s));
    auto tampered_packet = valid.protect(fake_kcp_packet);
    tampered_packet.back() ^= std::byte{1};
    send_udp(client.get(), server.local_port(), tampered_packet);
    server.poll_once(20ms);
    EXPECT_EQ(server.connection_count(), 0U);
}

TEST(GatewayConfigLoaderTest, LoadsProtocolSelectionFromLua) {
    const auto config = GatewayConfigLoader::load(
        std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) /
        "lua/config/services/gateway.lua");

    ASSERT_EQ(config.transports.size(), 3U);
    EXPECT_EQ(config.tick_rate, 20U);
    EXPECT_TRUE(config.service_discovery.enabled);
    EXPECT_FALSE(config.service_discovery.required);
    EXPECT_EQ(
        config.service_discovery.endpoint,
        "http://127.0.0.1:2379");
    EXPECT_EQ(config.service_discovery.instance_id, "gateway-dev-01");
    EXPECT_EQ(config.max_events_per_frame, 4096U);
    EXPECT_EQ(config.runtime.inbound_capacity, 65536U);
    EXPECT_EQ(config.runtime.outbound_capacity, 65536U);
    EXPECT_EQ(config.runtime.max_commands_per_cycle, 4096U);
    EXPECT_EQ(config.runtime.io_poll_interval, std::chrono::milliseconds(2));
    EXPECT_EQ(config.transports[0].name, "client_tcp");
    EXPECT_EQ(config.transports[0].protocol, network::TransportProtocol::Tcp);
    EXPECT_TRUE(config.transports[0].enabled);
    EXPECT_EQ(config.transports[1].protocol, network::TransportProtocol::Udp);
    EXPECT_FALSE(config.transports[1].enabled);
    EXPECT_EQ(config.transports[2].protocol, network::TransportProtocol::Kcp);
    EXPECT_FALSE(config.transports[2].enabled);
}

TEST(GatewayConfigLoaderTest, LoadsKcpKeyFromEnvironment) {
    constexpr auto key =
        "0102030405060708090a0b0c0d0e0f10"
        "1112131415161718191a1b1c1d1e1f20";
    ASSERT_EQ(::setenv("REALMMESH_TEST_KCP_KEY", key, 1), 0);
    const auto config = GatewayConfigLoader::load(
        std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) /
        "tests/cpp/fixtures/gateway_kcp_config.lua");
    static_cast<void>(::unsetenv("REALMMESH_TEST_KCP_KEY"));

    ASSERT_EQ(config.transports.size(), 1U);
    ASSERT_TRUE(config.transports[0].kcp_ticket_key.has_value());
    EXPECT_EQ(config.transports[0].kcp_ticket_key->front(), std::byte{1});
    EXPECT_EQ(config.transports[0].kcp_ticket_key->back(), std::byte{0x20});
    EXPECT_EQ(config.transports[0].idle_timeout, std::chrono::milliseconds(15000));
}

}  // namespace
}  // namespace realm::game::gateway
