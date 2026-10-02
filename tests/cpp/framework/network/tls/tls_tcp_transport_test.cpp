#include "realmmesh/network/client/tls_client_stream.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"
#include "realmmesh/observability/logger.hpp"

#include <gtest/gtest.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <nlohmann/json.hpp>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <csignal>
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
    void reset() noexcept {
        if (value_ >= 0) {
            ::close(value_);
            value_ = -1;
        }
    }

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
    EXPECT_FALSE(accepted->at("attributes").contains("peer_address"));
    EXPECT_FALSE(accepted->at("attributes").contains("peer_port"));

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

/// 对端 RST 后再次写入,内核默认以 SIGPIPE 终止进程。此处断言传输层把这次
/// 写入当作 I/O 失败返回 false,而非整个进程被信号杀掉——若防护缺失,测试
/// 二进制会被信号终止,用例失败。
TEST(TlsTcpTransportTest, WritingToAResetPeerFailsWithoutTerminatingProcess) {
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
    auto transports = TransportFactory::create_enabled(configs);
    ASSERT_EQ(transports.size(), 1U);
    auto& transport = *transports.front();

    std::atomic<SessionId> session_id{invalid_session_id};
    std::atomic<bool> session_ready{false};
    std::atomic<bool> peer_reset{false};
    std::atomic<bool> write_failed{false};

    std::jthread server([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline &&
               session_id.load() == invalid_session_id) {
            for (auto& event :
                 transport.poll_once(std::chrono::milliseconds(20))) {
                if (event.kind == TransportEventKind::MessageReceived) {
                    session_id = event.session_id;
                }
            }
        }
        if (session_id.load() == invalid_session_id) {
            return;
        }
        session_ready = true;

        // 等客户端把连接重置掉,再在"未通过事件循环观察到关闭"的状态下直接
        // 写入——这正是生产路径上会触发 SIGPIPE 的时刻。
        while (!peer_reset.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        const std::array<std::byte, 16> payload{};
        while (std::chrono::steady_clock::now() < deadline) {
            if (!transport.send(session_id.load(), payload)) {
                write_failed = true;
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
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
    const std::array<unsigned char, 17> alpn{
        16, 'r', 'e', 'a', 'l', 'm', 'm', 'e', 's', 'h', '-', 'e', 'd', 'g', 'e', '/', '1'};
    ASSERT_EQ(SSL_set_alpn_protos(ssl.get(), alpn.data(), alpn.size()), 0);
    ASSERT_EQ(SSL_connect(ssl.get()), 1);

    const std::array<std::byte, 4> payload{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    write_all(ssl.get(), frame_header(payload.size()));
    write_all(ssl.get(), payload);

    for (int attempt = 0; attempt < 500 && !session_ready.load(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(session_ready.load());

    // SO_LINGER 的 l_linger = 0 让 close() 直接发 RST,而不是正常的 FIN。
    const linger reset_linger{.l_onoff = 1, .l_linger = 0};
    ASSERT_EQ(
        ::setsockopt(
            socket.get(),
            SOL_SOCKET,
            SO_LINGER,
            &reset_linger,
            sizeof(reset_linger)),
        0);
    ssl.reset();
    socket.reset();
    peer_reset = true;

    server.join();
    EXPECT_TRUE(write_failed.load());
    EXPECT_EQ(transport.session_count(), 0U);
}


/// 客户端侧的同一风险:服务端关掉会话后客户端继续写(Realm 重启后的心跳就是
/// 这一刻)。macOS 上暂时恢复 SIGPIPE 的默认处置,确认保护来自客户端套接字
/// 本身(SO_NOSIGPIPE),而不是本二进制恰好链接进来的服务端进程级兜底;
/// 若防护缺失,测试二进制会被信号终止。
TEST(TlsTcpTransportTest, ClientWritingToAClosedServerFailsWithoutTerminatingProcess) {
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
    auto transports = TransportFactory::create_enabled(configs);
    ASSERT_EQ(transports.size(), 1U);
    auto& transport = *transports.front();

    std::atomic<bool> server_closed{false};
    std::jthread server([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& event :
                 transport.poll_once(std::chrono::milliseconds(20))) {
                if (event.kind == TransportEventKind::MessageReceived) {
                    static_cast<void>(transport.close(event.session_id));
                    server_closed = true;
                    return;
                }
            }
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto dialed = client::TlsClientStream::dial(
        "127.0.0.1", transport.local_endpoint().port,
        client::TlsClientOptions{.alpn = "realmmesh-edge/1",
                                 .verify_peer = false},
        deadline, std::stop_token{});
    ASSERT_TRUE(dialed.stream != nullptr);
    auto& stream = *dialed.stream;

    const std::array<std::byte, 4> payload{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    ASSERT_TRUE(stream.write_all(frame_header(payload.size()), deadline));
    ASSERT_TRUE(stream.write_all(payload, deadline));
    server.join();
    ASSERT_TRUE(server_closed.load());

#if defined(__APPLE__)
    struct DefaultSigpipe final {
        void (*previous)(int) = std::signal(SIGPIPE, SIG_DFL);
        ~DefaultSigpipe() { std::signal(SIGPIPE, previous); }
    } default_sigpipe;
#endif
    bool write_failed = false;
    while (!write_failed && std::chrono::steady_clock::now() < deadline) {
        write_failed = !stream.write_all(payload, deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(write_failed);
}

/// 同一线程上别的连接失败的 I/O(如写已被对端 RST 的套接字)会在 OpenSSL 线程
/// 错误队列里留下条目;SSL_get_error 先看队列,不清就会把健康连接的
/// WANT_READ 误判成致命错误。这里直接塞一条残留错误模拟那一刻。
void leave_stale_openssl_error() { ERR_raise(ERR_LIB_SYS, ECONNRESET); }

[[nodiscard]] std::vector<TransportConfig> edge_tls_configs() {
    return {{
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
}

/// 客户端:顶替后旧会话的失败 I/O 不得让同线程上的新会话读失败(#93 中
/// 被顶替会话关闭后,新会话的下一次请求被误报为 Disconnected)。
TEST(TlsTcpTransportTest, ClientReadIgnoresOpenSslErrorsLeftByAnotherConnection) {
    auto transports = TransportFactory::create_enabled(edge_tls_configs());
    ASSERT_EQ(transports.size(), 1U);
    auto& transport = *transports.front();

    const std::array<std::byte, 4> payload{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    std::jthread server([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (std::chrono::steady_clock::now() < deadline) {
            for (auto& event :
                 transport.poll_once(std::chrono::milliseconds(20))) {
                if (event.kind == TransportEventKind::MessageReceived) {
                    // 晚一点回包,让客户端先读到 WANT_READ。
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    static_cast<void>(
                        transport.send(event.session_id, event.payload));
                    static_cast<void>(
                        transport.poll_once(std::chrono::milliseconds(20)));
                    return;
                }
            }
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto dialed = client::TlsClientStream::dial(
        "127.0.0.1", transport.local_endpoint().port,
        client::TlsClientOptions{.alpn = "realmmesh-edge/1",
                                 .verify_peer = false},
        deadline, std::stop_token{});
    ASSERT_TRUE(dialed.stream != nullptr);
    auto& stream = *dialed.stream;
    ASSERT_TRUE(stream.write_all(frame_header(payload.size()), deadline));
    ASSERT_TRUE(stream.write_all(payload, deadline));

    leave_stale_openssl_error();
    std::array<std::byte, 8> echoed{};
    std::size_t received = 0;
    while (received < echoed.size()) {
        const auto read = stream.read_some(
            std::span{echoed}.subspan(received), deadline);
        ASSERT_TRUE(read.has_value()) << "read failed after " << received;
        ASSERT_GT(*read, 0U);
        received += *read;
    }
    EXPECT_TRUE(std::equal(
        payload.begin(), payload.end(), echoed.begin() + 4));
}

/// 服务端:一个事件循环线程服务所有连接,一个连接的失败不得让别的连接在
/// 握手或收包时被误判关闭。
TEST(TlsTcpTransportTest, ServerIgnoresOpenSslErrorsLeftByAnotherConnection) {
    auto transports = TransportFactory::create_enabled(edge_tls_configs());
    ASSERT_EQ(transports.size(), 1U);
    auto& transport = *transports.front();

    std::atomic<bool> opened{false};
    std::atomic<bool> received{false};
    std::atomic<bool> closed{false};
    std::jthread server([&] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!received && std::chrono::steady_clock::now() < deadline) {
            leave_stale_openssl_error();
            for (auto& event :
                 transport.poll_once(std::chrono::milliseconds(20))) {
                opened = opened ||
                         event.kind == TransportEventKind::SessionOpened;
                received = received ||
                           event.kind == TransportEventKind::MessageReceived;
                closed = closed ||
                         event.kind == TransportEventKind::SessionClosed;
            }
        }
    });

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto dialed = client::TlsClientStream::dial(
        "127.0.0.1", transport.local_endpoint().port,
        client::TlsClientOptions{.alpn = "realmmesh-edge/1",
                                 .verify_peer = false},
        deadline, std::stop_token{});
    ASSERT_TRUE(dialed.stream != nullptr);
    // 握手本身会清队列;隔几轮 poll 再发,帧落在一轮已留有残留错误的 poll 里。
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const std::array<std::byte, 4> payload{
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03}, std::byte{0x04}};
    ASSERT_TRUE(dialed.stream->write_all(frame_header(payload.size()), deadline));
    ASSERT_TRUE(dialed.stream->write_all(payload, deadline));
    server.join();

    EXPECT_TRUE(opened.load());
    EXPECT_TRUE(received.load());
    EXPECT_FALSE(closed.load());
}

}  // namespace
}  // namespace realm::network
