/// 服务边集成测试(规格缝 2,唯一动 socket 的缝):真实 TLS loopback,
/// HttpServer 起在临时端口、std::jthread 驱动 poll_once;主线程自建
/// OpenSSL 客户端(先例:tls_tcp_transport_test)断言 TLS 1.3 + ALPN
/// 协商、keep-alive 复用、协议错误回绝后关闭。

#include "realmmesh/network/http/http_server.hpp"

#include <gtest/gtest.h>
#include <openssl/ssl.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace realm::network {
namespace {

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

void write_all(SSL* ssl, std::string_view bytes) {
    const auto* data = reinterpret_cast<const std::byte*>(bytes.data());
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        std::size_t written = 0;
        ASSERT_EQ(
            SSL_write_ex(ssl, data + offset, bytes.size() - offset, &written),
            1);
        offset += written;
    }
}

/// 服务端每个响应都带 Content-Length(规格):读到响应体齐整为止。
[[nodiscard]] std::string read_response(SSL* ssl) {
    std::string wire;
    std::array<char, 2048> buffer{};
    const auto receive = [&](std::string& into) -> bool {
        std::size_t received = 0;
        if (SSL_read_ex(ssl, buffer.data(), buffer.size(), &received) != 1) {
            ADD_FAILURE() << "connection closed before response completed";
            return false;
        }
        into.append(buffer.data(), received);
        return true;
    };
    while (wire.find("\r\n\r\n") == std::string::npos) {
        if (!receive(wire)) {
            return wire;
        }
    }
    const auto head_end = wire.find("\r\n\r\n");
    std::size_t content_length = 0;
    std::istringstream head(wire.substr(0, head_end));
    std::string line;
    while (std::getline(head, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        constexpr std::string_view name = "Content-Length:";
        if (line.rfind(name, 0) == 0) {
            content_length = std::stoul(line.substr(name.size()));
        }
    }
    const std::size_t body_target = head_end + 4 + content_length;
    while (wire.size() < body_target) {
        if (!receive(wire)) {
            return wire;
        }
    }
    return wire;
}

/// 服务端回绝后关闭连接;服务端析构 TLS 对象不发送 close_notify,
/// 客户端按 OpenSSL 报零返回/系统层 EOF 处理皆可。
void expect_connection_closed(SSL* ssl) {
    std::array<char, 64> buffer{};
    std::size_t received = 0;
    const int result =
        SSL_read_ex(ssl, buffer.data(), buffer.size(), &received);
    EXPECT_NE(result, 1);
    if (result != 1) {
        const int error = SSL_get_error(ssl, result);
        EXPECT_TRUE(
            error == SSL_ERROR_ZERO_RETURN || error == SSL_ERROR_SYSCALL ||
            error == SSL_ERROR_SSL)
            << "unexpected SSL error " << error;
    }
}

class TlsClient final {
public:
    void connect(std::uint16_t port) {
        const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
        ASSERT_GE(descriptor, 0);
        socket_ = std::make_unique<Descriptor>(descriptor);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        ASSERT_EQ(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr), 1);
        ASSERT_EQ(
            ::connect(
                socket_->get(),
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)),
            0);

        context_.reset(SSL_CTX_new(TLS_client_method()));
        ASSERT_NE(context_, nullptr);
        ASSERT_EQ(
            SSL_CTX_set_min_proto_version(context_.get(), TLS1_3_VERSION), 1);
        ASSERT_EQ(
            SSL_CTX_set_max_proto_version(context_.get(), TLS1_3_VERSION), 1);
        ASSERT_EQ(
            SSL_CTX_load_verify_locations(
                context_.get(), REALMMESH_TEST_TLS_CERTIFICATE, nullptr),
            1);
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);

        ssl_.reset(SSL_new(context_.get()));
        ASSERT_NE(ssl_, nullptr);
        ASSERT_EQ(SSL_set_fd(ssl_.get(), socket_->get()), 1);
        ASSERT_EQ(SSL_set_tlsext_host_name(ssl_.get(), "localhost"), 1);
        ASSERT_EQ(SSL_set1_host(ssl_.get(), "localhost"), 1);
        static constexpr std::array<unsigned char, 9> alpn{
            8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
        ASSERT_EQ(SSL_set_alpn_protos(ssl_.get(), alpn.data(), alpn.size()), 0);
        ASSERT_EQ(SSL_connect(ssl_.get()), 1);
    }

    void assert_negotiated_http1_over_tls13() {
        EXPECT_EQ(SSL_version(ssl_.get()), TLS1_3_VERSION);
        const unsigned char* selected_alpn = nullptr;
        unsigned int selected_alpn_size = 0;
        SSL_get0_alpn_selected(ssl_.get(), &selected_alpn, &selected_alpn_size);
        EXPECT_EQ(
            std::string(
                reinterpret_cast<const char*>(selected_alpn),
                selected_alpn_size),
            "http/1.1");
    }

    void send(std::string_view wire) { write_all(ssl_.get(), wire); }

    [[nodiscard]] std::string read_response() {
        return ::realm::network::read_response(ssl_.get());
    }

    void expect_closed() { expect_connection_closed(ssl_.get()); }

private:
    std::unique_ptr<Descriptor> socket_;
    std::unique_ptr<SSL_CTX, SslContextDeleter> context_;
    std::unique_ptr<SSL, SslDeleter> ssl_;
};

struct CapturedRequest final {
    std::string method;
    std::string target;
    std::string host;
    std::string body;
};

std::jthread run_server(HttpServer& server, std::atomic<bool>& stop) {
    return std::jthread([&server, &stop] {
        const auto deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!stop.load() && std::chrono::steady_clock::now() < deadline) {
            server.poll_once(std::chrono::milliseconds(20));
        }
    });
}

TEST(HttpServerTest, ServesKeepAliveRequestsOverTls13WithAlpn) {
    std::vector<CapturedRequest> requests;
    std::mutex capture_mutex;
    auto handler = [&](const Http1Request& request) -> Http1Response {
        {
            const std::lock_guard lock(capture_mutex);
            requests.push_back({
                .method = request.method,
                .target = request.target,
                .host =
                    request.header("host") == nullptr
                        ? std::string{}
                        : *request.header("host"),
                .body = request.body,
            });
        }
        if (request.method == "GET") {
            return {.status = 200, .headers = {}, .body = "queue-progress"};
        }
        return {.status = 202, .headers = {}, .body = "verify-accepted"};
    };

    HttpServerConfig config;
    config.tls_identity = {
        .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
        .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
        .alpn = "http/1.1",
    };
    HttpServer server("127.0.0.1", 0, config, handler);
    std::atomic<bool> stop{false};
    std::jthread server_thread = run_server(server, stop);

    TlsClient client;
    ASSERT_NO_FATAL_FAILURE(client.connect(server.local_port()));
    client.assert_negotiated_http1_over_tls13();

    client.send("GET /v1/queue/progress HTTP/1.1\r\n"
                "Host: login.realmmesh.example\r\n"
                "\r\n");
    const auto first = client.read_response();
    EXPECT_EQ(first.find("HTTP/1.1 200 "), 0U);
    EXPECT_NE(first.find("Content-Length: 14"), std::string::npos);
    EXPECT_NE(first.find("Connection: keep-alive"), std::string::npos);
    EXPECT_NE(first.rfind("queue-progress"), std::string::npos);

    // 同一连接上第二个请求:keep-alive 复用(规格 §3 keep-alive)。
    client.send("POST /v1/login/verify HTTP/1.1\r\n"
                "Host: login.realmmesh.example\r\n"
                "Content-Length: 19\r\n"
                "\r\n"
                "{\"account\":\"edwin\"}");
    const auto second = client.read_response();
    EXPECT_EQ(second.find("HTTP/1.1 202 "), 0U);
    EXPECT_NE(second.rfind("verify-accepted"), std::string::npos);

    stop = true;
    server_thread.join();

    ASSERT_EQ(requests.size(), 2U);
    EXPECT_EQ(requests[0].method, "GET");
    EXPECT_EQ(requests[0].target, "/v1/queue/progress");
    EXPECT_EQ(requests[0].host, "login.realmmesh.example");
    EXPECT_EQ(requests[0].body, "");
    EXPECT_EQ(requests[1].method, "POST");
    EXPECT_EQ(requests[1].target, "/v1/login/verify");
    EXPECT_EQ(requests[1].body, "{\"account\":\"edwin\"}");
}

/// HTTP/1.0 请求 → 505 响应 + 连接关闭(规格:协议层错误由服务边回绝,
/// 不进 handler,响应后必须关闭)。
TEST(HttpServerTest, RejectsUnsupportedVersionThenCloses) {
    HttpServerConfig config;
    config.tls_identity = {
        .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
        .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
        .alpn = "http/1.1",
    };
    HttpServer server(
        "127.0.0.1",
        0,
        config,
        [](const Http1Request&) -> Http1Response {
            return {.status = 200, .headers = {}, .body = "ok"};
        });
    std::atomic<bool> stop{false};
    std::jthread server_thread = run_server(server, stop);

    TlsClient client;
    ASSERT_NO_FATAL_FAILURE(client.connect(server.local_port()));
    client.send("GET / HTTP/1.0\r\n"
                "Host: login.realmmesh.example\r\n"
                "\r\n");
    const auto response = client.read_response();
    EXPECT_EQ(response.find("HTTP/1.1 505 "), 0U);
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
    client.expect_closed();

    stop = true;
    server_thread.join();
}

/// Connection: close 请求 → 响应携带 Connection: close,服务端响应后
/// 主动关闭(规格 §3 响应序列化)。
TEST(HttpServerTest, HonorsClientRequestedClose) {
    HttpServerConfig config;
    config.tls_identity = {
        .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
        .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY,
        .alpn = "http/1.1",
    };
    HttpServer server(
        "127.0.0.1",
        0,
        config,
        [](const Http1Request&) -> Http1Response {
            return {.status = 200, .headers = {}, .body = "ok"};
        });
    std::atomic<bool> stop{false};
    std::jthread server_thread = run_server(server, stop);

    TlsClient client;
    ASSERT_NO_FATAL_FAILURE(client.connect(server.local_port()));
    client.send("GET /v1/health HTTP/1.1\r\n"
                "Host: login.realmmesh.example\r\n"
                "Connection: close\r\n"
                "\r\n");
    const auto response = client.read_response();
    EXPECT_EQ(response.find("HTTP/1.1 200 "), 0U);
    EXPECT_NE(response.find("Connection: close"), std::string::npos);
    client.expect_closed();

    stop = true;
    server_thread.join();
}

}  // namespace
}  // namespace realm::network
