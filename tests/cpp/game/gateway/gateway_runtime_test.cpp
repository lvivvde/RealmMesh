#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include "realmmesh/network/codec/length_field_codec.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace realm::game::gateway {
namespace {

struct ContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};
struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

class TlsClient final {
public:
    explicit TlsClient(std::uint16_t port)
        : descriptor_(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0)),
          context_(SSL_CTX_new(TLS_client_method())) {
        if (descriptor_ < 0 || !context_) {
            throw std::runtime_error("failed to create TLS test client");
        }
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (::connect(
                descriptor_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) < 0) {
            throw std::runtime_error("failed to connect TLS test client");
        }
        if (SSL_CTX_load_verify_locations(
                context_.get(), REALMMESH_TEST_TLS_CERTIFICATE, nullptr) != 1) {
            throw std::runtime_error("failed to load test CA");
        }
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
        ssl_.reset(SSL_new(context_.get()));
        const std::array<unsigned char, 17> alpn{
            16, 'r', 'e', 'a', 'l', 'm', 'm', 'e', 's', 'h', '-', 'e', 'd', 'g', 'e', '/', '1'};
        if (!ssl_ || SSL_set_fd(ssl_.get(), descriptor_) != 1 ||
            SSL_set_tlsext_host_name(ssl_.get(), "localhost") != 1 ||
            SSL_set1_host(ssl_.get(), "localhost") != 1 ||
            SSL_set_alpn_protos(ssl_.get(), alpn.data(), alpn.size()) != 0 ||
            SSL_connect(ssl_.get()) != 1) {
            throw std::runtime_error("TLS test handshake failed");
        }
    }

    ~TlsClient() { if (descriptor_ >= 0) ::close(descriptor_); }
    TlsClient(const TlsClient&) = delete;
    TlsClient& operator=(const TlsClient&) = delete;

    void send(std::span<const std::byte> bytes) {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            std::size_t written = 0;
            if (SSL_write_ex(
                    ssl_.get(),
                    bytes.data() + offset,
                    bytes.size() - offset,
                    &written) != 1) {
                throw std::runtime_error("TLS test write failed");
            }
            offset += written;
        }
    }

    [[nodiscard]] std::vector<std::byte> receive(std::size_t size) {
        std::vector<std::byte> result(size);
        std::size_t offset = 0;
        while (offset < size) {
            std::size_t received = 0;
            if (SSL_read_ex(
                    ssl_.get(),
                    result.data() + offset,
                    result.size() - offset,
                    &received) != 1) {
                throw std::runtime_error("TLS test read failed");
            }
            offset += received;
        }
        return result;
    }

private:
    int descriptor_;
    std::unique_ptr<SSL_CTX, ContextDeleter> context_;
    std::unique_ptr<SSL, SslDeleter> ssl_;
};

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    std::ranges::transform(text, std::back_inserter(result), [](char value) {
        return static_cast<std::byte>(value);
    });
    return result;
}

network::TransportConfig tls_transport() {
    return {
        .name = "client_tls_tcp",
        .protocol = network::TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .max_sessions = 16,
        .max_payload_size = 1024,
        .tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
            .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
        },
    };
}

TEST(GatewayRuntimeTest, AtomicallyRespondsAndPromotesAPendingConnection) {
    using namespace std::chrono_literals;
    GatewayRuntime runtime(
        {.transports = {tls_transport()}},
        {
            .inbound_capacity = 16,
            .outbound_capacity = 16,
            .io_poll_interval = 1ms,
        });
    runtime.start();
    TlsClient client(runtime.local_port());
    const network::LengthFieldCodec codec(1024);
    const auto payload = bytes("frame-message");
    client.send(codec.encode(payload));

    std::optional<GatewayEvent> message;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (std::chrono::steady_clock::now() < deadline && !message) {
        auto event = runtime.try_receive();
        if (event && event->kind == GatewayEventKind::MessageReceived) {
            message = std::move(event);
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    ASSERT_TRUE(message.has_value());
    EXPECT_FALSE(message->client_session_id.has_value());

    const auto accepted = bytes("accepted");
    EXPECT_EQ(
        runtime.try_accept_connection(
            message->transport_name,
            message->transport_session_id,
            accepted),
        QueueResult::Queued);
    EXPECT_EQ(client.receive(codec.encode(accepted).size()), codec.encode(accepted));

    std::optional<GatewayEvent> promoted;
    while (std::chrono::steady_clock::now() < deadline && !promoted) {
        auto event = runtime.try_receive();
        if (event && event->kind == GatewayEventKind::ClientSessionOpened) {
            promoted = std::move(event);
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
    ASSERT_TRUE(promoted.has_value());
    ASSERT_TRUE(promoted->client_session_id.has_value());
    EXPECT_EQ(
        runtime.try_send(*promoted->client_session_id, payload),
        QueueResult::Queued);
    EXPECT_EQ(client.receive(codec.encode(payload).size()), codec.encode(payload));
    EXPECT_EQ(runtime.stats().successful_deliveries, 2U);
    runtime.stop();
}

TEST(GatewayRuntimeTest, RejectsCommandsWhileStopped) {
    GatewayRuntime runtime(
        {.transports = {tls_transport()}},
        {.inbound_capacity = 1, .outbound_capacity = 1});
    EXPECT_EQ(runtime.try_send(1, bytes("ignored")), QueueResult::Stopped);
}

}  // namespace
}  // namespace realm::game::gateway
