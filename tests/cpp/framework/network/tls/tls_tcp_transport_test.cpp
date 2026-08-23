#include "realmmesh/network/transport/transport_factory.hpp"
#include "realmmesh/observability/logger.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace realm::network {
namespace {

class TemporaryLogFile final {
public:
    TemporaryLogFile()
        : path_(
              std::filesystem::temp_directory_path() /
              ("realmmesh-transport-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()) +
               ".jsonl")) {}

    ~TemporaryLogFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<nlohmann::json> read_log_events(
    const std::filesystem::path& path) {
    std::ifstream input(path);
    std::vector<nlohmann::json> events;
    std::string line;
    while (std::getline(input, line)) {
        events.push_back(nlohmann::json::parse(line));
    }
    return events;
}

[[nodiscard]] const nlohmann::json* find_log_event(
    const std::vector<nlohmann::json>& events, std::string_view event_name) {
    const auto iterator =
        std::ranges::find_if(events, [event_name](const auto& event) {
            return event.at("event_name") == event_name;
        });
    return iterator == events.end() ? nullptr : &*iterator;
}

class Descriptor final {
public:
    explicit Descriptor(int value)
        : value_(value) {}
    ~Descriptor() {
        if (value_ >= 0) ::close(value_);
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    [[nodiscard]] int get() const noexcept { return value_; }

private:
    int value_;
};

struct SslContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};
struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

[[nodiscard]] std::array<std::byte, 4> frame_header(std::size_t size) {
    const auto value = static_cast<std::uint32_t>(size);
    return {
        static_cast<std::byte>((value >> 24U) & 0xFFU),
        static_cast<std::byte>((value >> 16U) & 0xFFU),
        static_cast<std::byte>((value >> 8U) & 0xFFU),
        static_cast<std::byte>(value & 0xFFU),
    };
}

void write_all(SSL* ssl, std::span<const std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t written = 0;
        ASSERT_EQ(
            SSL_write_ex(
                ssl, bytes.data() + offset, bytes.size() - offset, &written),
            1);
        offset += written;
    }
}

void read_all(SSL* ssl, std::span<std::byte> bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t received = 0;
        ASSERT_EQ(
            SSL_read_ex(
                ssl, bytes.data() + offset, bytes.size() - offset, &received),
            1);
        offset += received;
    }
}

TEST(TlsTcpTransportTest, NegotiatesTls13AndAlpnBeforeExchangingFrames) {
    TemporaryLogFile log_file;
    observability::LoggerConfig logger_config;
    logger_config.file_path = log_file.path();
    observability::Logger logger(
        logger_config,
        observability::ServiceIdentity{.service_name = "gateway"});
    const std::vector<TransportConfig> configs{{
        .name = "client_tls",
        .protocol = TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .tls =
            TransportConfig::TlsServerIdentity{
                .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
                .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
                .alpn = "realmmesh-edge/1",
            },
    }};
    auto transports = TransportFactory::create_enabled(configs, &logger);
    ASSERT_EQ(transports.size(), 1U);
    auto& transport = *transports.front();

    std::atomic<bool> received{false};
    std::atomic<SessionId> session_id{invalid_session_id};
    std::jthread server([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& event :
                 transport.poll_once(std::chrono::milliseconds(20))) {
                if (event.kind == TransportEventKind::MessageReceived) {
                    session_id = event.session_id;
                    received = true;
                    EXPECT_TRUE(
                        transport.send(event.session_id, event.payload));
                    return;
                }
            }
        }
    });

    Descriptor socket(::socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_GE(socket.get(), 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(transport.local_endpoint().port);
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
    ASSERT_EQ(
        ::connect(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0);

    std::unique_ptr<SSL_CTX, SslContextDeleter> context(
        SSL_CTX_new(TLS_client_method()));
    ASSERT_NE(context, nullptr);
    ASSERT_EQ(SSL_CTX_set_min_proto_version(context.get(), TLS1_3_VERSION), 1);
    ASSERT_EQ(SSL_CTX_set_max_proto_version(context.get(), TLS1_3_VERSION), 1);
    ASSERT_EQ(
        SSL_CTX_load_verify_locations(
            context.get(), REALMMESH_TEST_TLS_CERTIFICATE, nullptr),
        1);
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_PEER, nullptr);

    std::unique_ptr<SSL, SslDeleter> ssl(SSL_new(context.get()));
    ASSERT_NE(ssl, nullptr);
    ASSERT_EQ(SSL_set_fd(ssl.get(), socket.get()), 1);
    ASSERT_EQ(SSL_set_tlsext_host_name(ssl.get(), "localhost"), 1);
    ASSERT_EQ(SSL_set1_host(ssl.get(), "localhost"), 1);
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
    ASSERT_EQ(SSL_set_alpn_protos(ssl.get(), alpn.data(), alpn.size()), 0);
    ASSERT_EQ(SSL_connect(ssl.get()), 1);
    EXPECT_EQ(SSL_version(ssl.get()), TLS1_3_VERSION);
    const unsigned char* selected_alpn = nullptr;
    unsigned int selected_alpn_size = 0;
    SSL_get0_alpn_selected(ssl.get(), &selected_alpn, &selected_alpn_size);
    EXPECT_EQ(
        std::string(
            reinterpret_cast<const char*>(selected_alpn), selected_alpn_size),
        "realmmesh-edge/1");

    const std::array<std::byte, 4> payload{
        std::byte{0x10}, std::byte{0x20}, std::byte{0x30}, std::byte{0x40}};
    const auto header = frame_header(payload.size());
    write_all(ssl.get(), header);
    write_all(ssl.get(), payload);

    std::array<std::byte, 8> response{};
    read_all(ssl.get(), response);
    EXPECT_TRUE(
        std::equal(payload.begin(), payload.end(), response.begin() + 4));

    server.join();
    EXPECT_TRUE(received.load());
    ASSERT_NE(session_id.load(), invalid_session_id);
    ASSERT_TRUE(transport.close(session_id.load()));
    ASSERT_TRUE(logger.flush(std::chrono::seconds(2)));

    const auto log_events = read_log_events(log_file.path());
    const auto* accepted = find_log_event(log_events, "connection_accepted");
    ASSERT_NE(accepted, nullptr);
    EXPECT_EQ(accepted->at("attributes").at("protocol"), "tls_tcp");
    EXPECT_EQ(accepted->at("attributes").at("peer_address"), "127.0.0.1");
    EXPECT_GT(accepted->at("attributes").at("peer_port").get<int>(), 0);

    const auto* handshake =
        find_log_event(log_events, "tls_handshake_completed");
    ASSERT_NE(handshake, nullptr);
    EXPECT_EQ(handshake->at("attributes").at("alpn"), "realmmesh-edge/1");

    const auto* closed = find_log_event(log_events, "connection_closed");
    ASSERT_NE(closed, nullptr);
    EXPECT_EQ(closed->at("attributes").at("reason"), "application_requested");
}

TEST(TlsTcpTransportTest, DoesNotOpenASessionWithoutRequiredAlpn) {
    const std::vector<TransportConfig> configs{{
        .name = "client_tls",
        .protocol = TransportProtocol::TlsTcp,
        .listen_address = "127.0.0.1",
        .listen_port = 0,
        .tls =
            TransportConfig::TlsServerIdentity{
                .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
                .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
            },
    }};
    auto transports = TransportFactory::create_enabled(configs);
    auto& transport = *transports.front();

    std::jthread server([&] {
        for (int attempt = 0; attempt < 100; ++attempt) {
            static_cast<void>(
                transport.poll_once(std::chrono::milliseconds(10)));
        }
    });

    Descriptor socket(::socket(AF_INET, SOCK_STREAM, 0));
    ASSERT_GE(socket.get(), 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(transport.local_endpoint().port);
    ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
    ASSERT_EQ(
        ::connect(
            socket.get(),
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)),
        0);

    std::unique_ptr<SSL_CTX, SslContextDeleter> context(
        SSL_CTX_new(TLS_client_method()));
    ASSERT_NE(context, nullptr);
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);
    std::unique_ptr<SSL, SslDeleter> ssl(SSL_new(context.get()));
    ASSERT_NE(ssl, nullptr);
    ASSERT_EQ(SSL_set_fd(ssl.get(), socket.get()), 1);
    EXPECT_EQ(SSL_connect(ssl.get()), 1);
    server.join();
    EXPECT_EQ(transport.session_count(), 0U);
}

}  // namespace
}  // namespace realm::network
