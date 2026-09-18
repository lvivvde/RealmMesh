#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include "realmmesh/network/codec/length_field_codec.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>
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
        : descriptor_(::socket(AF_INET, SOCK_STREAM, 0)),
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
            16,
            'r',
            'e',
            'a',
            'l',
            'm',
            'm',
            'e',
            's',
            'h',
            '-',
            'e',
            'd',
            'g',
            'e',
            '/',
            '1'};
        if (!ssl_ || SSL_set_fd(ssl_.get(), descriptor_) != 1 ||
            SSL_set_tlsext_host_name(ssl_.get(), "localhost") != 1 ||
            SSL_set1_host(ssl_.get(), "localhost") != 1 ||
            SSL_set_alpn_protos(ssl_.get(), alpn.data(), alpn.size()) != 0 ||
            SSL_connect(ssl_.get()) != 1) {
            throw std::runtime_error("TLS test handshake failed");
        }
    }

    ~TlsClient() {
        if (descriptor_ >= 0) ::close(descriptor_);
    }
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

    /// 服务端关闭后的读取:返回 true 表示连接已终结(FIN/RST/关闭通知)。
    [[nodiscard]] bool reached_eof() {
        std::array<std::byte, 1> buffer{};
        std::size_t received = 0;
        return SSL_read_ex(
                   ssl_.get(), buffer.data(), buffer.size(), &received) == 0;
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
        .tls =
            network::TransportConfig::TlsServerIdentity{
                .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
                .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
            },
    };
}

std::optional<GatewayEvent> wait_for_event(
    GatewayRuntime& runtime,
    GatewayEventKind kind,
    std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    std::optional<GatewayEvent> found;
    while (std::chrono::steady_clock::now() < deadline && !found) {
        auto event = runtime.try_receive();
        if (event && event->kind == kind) {
            found = std::move(event);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return found;
}

template <typename Predicate>
bool wait_until(
    Predicate&& predicate, std::chrono::milliseconds budget) {
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return predicate();
}

/// 连接 → 等握手(SessionOpened)→ 发一帧 → 等首个 MessageReceived。
/// 客户端连接由调用方持有,保证会话在其存活期内可继续收发。
struct ConnectedClient {
    std::unique_ptr<TlsClient> client;
    std::optional<GatewayEvent> message;
};

ConnectedClient connect_and_send_first_message(
    GatewayRuntime& runtime,
    const network::LengthFieldCodec& codec,
    std::string_view message_text) {
    ConnectedClient connected{std::make_unique<TlsClient>(runtime.local_port()),
                              std::nullopt};
    connected.client->send(codec.encode(bytes(message_text)));

    const auto opened = wait_for_event(
        runtime, GatewayEventKind::SessionOpened, std::chrono::seconds(2));
    EXPECT_NE(opened->session_id, invalid_edge_session_id);
    EXPECT_FALSE(opened->established);

    connected.message = wait_for_event(
        runtime, GatewayEventKind::MessageReceived, std::chrono::seconds(2));
    return connected;
}

TEST(GatewayRuntimeTest, AcceptRespondsAndEstablishesAtomically) {
    using namespace std::chrono_literals;
    GatewayRuntime runtime(
        {.transports = {tls_transport()}},
        {
            .inbound_capacity = 16,
            .outbound_capacity = 16,
            .io_poll_interval = 1ms,
        });
    runtime.start();
    const network::LengthFieldCodec codec(1024);
    auto connected =
        connect_and_send_first_message(runtime, codec, "frame-message");
    ASSERT_TRUE(connected.message.has_value());
    EXPECT_FALSE(connected.message->established);

    const auto accepted = bytes("accepted");
    EXPECT_EQ(
        runtime.try_accept(connected.message->session_id, accepted),
        QueueResult::Queued);
    EXPECT_EQ(
        connected.client->receive(codec.encode(accepted).size()),
        codec.encode(accepted));

    const auto established = wait_for_event(
        runtime, GatewayEventKind::SessionEstablished, std::chrono::seconds(2));
    ASSERT_TRUE(established.has_value());
    EXPECT_EQ(established->session_id, connected.message->session_id);
    EXPECT_TRUE(established->established);

    const auto payload = bytes("frame-message");
    const auto encoded_payload = codec.encode(payload);
    EXPECT_EQ(
        runtime.try_send(connected.message->session_id, payload),
        QueueResult::Queued);
    EXPECT_EQ(
        connected.client->receive(encoded_payload.size()), encoded_payload);
    EXPECT_EQ(runtime.stats().successful_deliveries, 2U);
    EXPECT_EQ(runtime.stats().unknown_session_commands, 0U);
    runtime.stop();
}

TEST(GatewayRuntimeTest, EstablishedPeerClosePublishesRealSessionClosed) {
    using namespace std::chrono_literals;
    GatewayRuntime runtime(
        {.transports = {tls_transport()}},
        {
            .inbound_capacity = 16,
            .outbound_capacity = 16,
            .io_poll_interval = 1ms,
        });
    runtime.start();
    const network::LengthFieldCodec codec(1024);
    auto connected =
        connect_and_send_first_message(runtime, codec, "frame-message");
    ASSERT_TRUE(connected.message.has_value());

    const auto accepted = bytes("accepted");
    ASSERT_EQ(
        runtime.try_accept(connected.message->session_id, accepted),
        QueueResult::Queued);
    ASSERT_EQ(
        connected.client->receive(codec.encode(accepted).size()),
        codec.encode(accepted));
    ASSERT_TRUE(wait_for_event(
                    runtime,
                    GatewayEventKind::SessionEstablished,
                    std::chrono::seconds(2))
                    .has_value());

    const auto session_id = connected.message->session_id;
    connected.client.reset();
    const auto closed = wait_for_event(
        runtime, GatewayEventKind::SessionClosed, std::chrono::seconds(2));
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->session_id, session_id);
    EXPECT_TRUE(closed->established);
    runtime.stop();
}

TEST(GatewayRuntimeTest, DeclineRejectsAndTerminatesPendingSession) {
    using namespace std::chrono_literals;
    GatewayRuntime runtime(
        {.transports = {tls_transport()}},
        {
            .inbound_capacity = 16,
            .outbound_capacity = 16,
            .io_poll_interval = 1ms,
        });
    runtime.start();
    const network::LengthFieldCodec codec(1024);
    auto connected =
        connect_and_send_first_message(runtime, codec, "frame-message");
    ASSERT_TRUE(connected.message.has_value());

    const auto rejected = bytes("rejected");
    EXPECT_EQ(
        runtime.try_decline(connected.message->session_id, rejected),
        QueueResult::Queued);
    EXPECT_EQ(
        connected.client->receive(codec.encode(rejected).size()),
        codec.encode(rejected));

    // 本地关闭不依赖传输层上报:终结事件由 runtime 合成,恰好一次。
    const auto closed = wait_for_event(
        runtime, GatewayEventKind::SessionClosed, std::chrono::seconds(2));
    ASSERT_TRUE(closed.has_value());
    EXPECT_EQ(closed->session_id, connected.message->session_id);
    EXPECT_FALSE(closed->established);
    EXPECT_TRUE(connected.client->reached_eof());
    EXPECT_EQ(runtime.stats().successful_deliveries, 1U);
    runtime.stop();
}

TEST(GatewayRuntimeTest, SendsToPendingSessionsCountAsUnknown) {
    using namespace std::chrono_literals;
    GatewayRuntime runtime(
        {.transports = {tls_transport()}},
        {
            .inbound_capacity = 16,
            .outbound_capacity = 16,
            .io_poll_interval = 1ms,
        });
    runtime.start();
    const network::LengthFieldCodec codec(1024);
    auto connected =
        connect_and_send_first_message(runtime, codec, "frame-message");
    ASSERT_TRUE(connected.message.has_value());

    EXPECT_EQ(
        runtime.try_send(connected.message->session_id, bytes("early")),
        QueueResult::Queued);
    EXPECT_TRUE(wait_until(
        [&runtime] { return runtime.stats().unknown_session_commands == 1; },
        2s));
    EXPECT_EQ(runtime.stats().successful_deliveries, 0U);
    runtime.stop();
}

TEST(GatewayRuntimeTest, CommandsForUnknownSessionsCountAsUnknown) {
    using namespace std::chrono_literals;
    GatewayRuntime runtime(
        {.transports = {tls_transport()}},
        {
            .inbound_capacity = 16,
            .outbound_capacity = 16,
            .io_poll_interval = 1ms,
        });
    runtime.start();

    EXPECT_EQ(
        runtime.try_accept(EdgeSessionId{4242}, bytes("x")),
        QueueResult::Queued);
    EXPECT_EQ(runtime.try_close(EdgeSessionId{4243}), QueueResult::Queued);
    EXPECT_TRUE(wait_until(
        [&runtime] { return runtime.stats().unknown_session_commands == 2; },
        2s));
    runtime.stop();
}

TEST(GatewayRuntimeTest, RejectsCommandsWhileStopped) {
    GatewayRuntime runtime(
        {.transports = {tls_transport()}},
        {.inbound_capacity = 1, .outbound_capacity = 1});
    EXPECT_EQ(
        runtime.try_send(EdgeSessionId{1}, bytes("ignored")),
        QueueResult::Stopped);
    EXPECT_EQ(
        runtime.try_accept(EdgeSessionId{1}, bytes("ignored")),
        QueueResult::Stopped);
    EXPECT_EQ(
        runtime.try_decline(EdgeSessionId{1}, bytes("ignored")),
        QueueResult::Stopped);
    EXPECT_EQ(runtime.try_close(EdgeSessionId{1}), QueueResult::Stopped);
}

}  // namespace
}  // namespace realm::game::gateway
