#include "realmmesh/loadgen/http_client.hpp"

#include "realmmesh/network/core/byte_buffer.hpp"
#include "realmmesh/network/http/http1_response_parser.hpp"

#include <openssl/ssl.h>

#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace realm::loadgen {
namespace {

struct SslContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};
struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

[[nodiscard]] int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
        deadline - std::chrono::steady_clock::now());
    return remaining.count() <= 0 ? 0
                                  : static_cast<int>(remaining.count());
}

/// 非阻塞等待 SSL 想要的 IO 方向;超时返回 false。
[[nodiscard]] bool wait_ssl_ready(
    int descriptor,
    int ssl_error,
    std::chrono::steady_clock::time_point deadline) {
    const short events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
    std::array<::pollfd, 1> fds{{{descriptor, events, 0}}};
    const int timeout = remaining_ms(deadline);
    if (timeout <= 0) {
        return false;
    }
    return ::poll(fds.data(), 1, timeout) > 0;
}

}  // namespace

struct TlsHttpConnection::Impl final {
    int descriptor{-1};
    std::unique_ptr<SSL_CTX, SslContextDeleter> context;
    std::unique_ptr<SSL, SslDeleter> ssl;
    network::Http1ResponseParser parser;

    ~Impl() {
        if (ssl != nullptr) {
            static_cast<void>(SSL_shutdown(ssl.get()));
        }
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
    }

    /// 非阻塞写完全部字节;失败返回 false。
    [[nodiscard]] bool write_all(
        const unsigned char* data,
        std::size_t size,
        std::chrono::steady_clock::time_point deadline) {
        std::size_t offset = 0;
        while (offset < size) {
            std::size_t written = 0;
            if (SSL_write_ex(
                    ssl.get(), data + offset, size - offset, &written) == 1) {
                offset += written;
                continue;
            }
            const int error = SSL_get_error(ssl.get(), 0);
            if ((error == SSL_ERROR_WANT_READ ||
                 error == SSL_ERROR_WANT_WRITE) &&
                wait_ssl_ready(descriptor, error, deadline)) {
                continue;
            }
            return false;
        }
        return true;
    }
};


std::unique_ptr<TlsHttpConnection> TlsHttpConnection::dial(
    const ServiceAddress& address,
    std::chrono::steady_clock::time_point deadline) {
    ::addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo* resolved = nullptr;
    if (::getaddrinfo(
            address.host.c_str(),
            std::to_string(address.port).c_str(),
            &hints,
            &resolved) != 0 ||
        resolved == nullptr) {
        return nullptr;
    }
    auto impl = std::make_unique<Impl>();
    impl->descriptor = ::socket(
        resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (impl->descriptor < 0) {
        ::freeaddrinfo(resolved);
        return nullptr;
    }
    // 压测工具的关闭语义:SO_LINGER 0 = 关闭即 RST,不进 TIME_WAIT。
    // 万级机器人 × 每机器人数条连接的短连洪峰会把本机临时端口泡进
    // TIME_WAIT(macOS 默认段约 16k),端口耗尽后 dial 全线失败;响应
    // 读完才关,无数据丢失窗口。
    const struct linger reset_close{1, 0};
    static_cast<void>(::setsockopt(
        impl->descriptor,
        SOL_SOCKET,
        SO_LINGER,
        &reset_close,
        sizeof(reset_close)));
    // 非阻塞拨号:连接与握手全程受截止约束,坏地址不挂死机器人。
    const int flags = ::fcntl(impl->descriptor, F_GETFL, 0);
    if (flags < 0 ||
        ::fcntl(impl->descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::freeaddrinfo(resolved);
        return nullptr;
    }
    const int connected =
        ::connect(impl->descriptor, resolved->ai_addr, resolved->ai_addrlen);
    ::freeaddrinfo(resolved);
    if (connected != 0) {
        if (errno != EINPROGRESS) {
            return nullptr;
        }
        std::array<::pollfd, 1> fds{{{impl->descriptor, POLLOUT, 0}}};
        const int timeout = remaining_ms(deadline);
        if (timeout <= 0 || ::poll(fds.data(), 1, timeout) != 1) {
            return nullptr;
        }
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        if (::getsockopt(
                impl->descriptor,
                SOL_SOCKET,
                SO_ERROR,
                &socket_error,
                &error_size) != 0 ||
            socket_error != 0) {
            return nullptr;
        }
    }

    impl->context.reset(SSL_CTX_new(TLS_client_method()));
    // 压测对象是自签证书环境(测试/soak 档):不校验服务端证书,
    // 应用层凭据(token)承担身份;诊断工具不收私有输入。
    SSL_CTX_set_verify(impl->context.get(), SSL_VERIFY_NONE, nullptr);
    impl->ssl.reset(SSL_new(impl->context.get()));
    // 服务端握手要求协商出 ALPN(http/1.1),不带即被回绝关闭。
    static constexpr std::array<unsigned char, 9> alpn{
        8, 'h', 't', 't', 'p', '/', '1', '.', '1'};
    if (impl->ssl == nullptr ||
        SSL_set_fd(impl->ssl.get(), impl->descriptor) != 1 ||
        SSL_set_tlsext_host_name(impl->ssl.get(), address.host.c_str()) != 1 ||
        SSL_set_alpn_protos(impl->ssl.get(), alpn.data(), alpn.size()) != 0) {
        return nullptr;
    }
    for (;;) {
        const int result = SSL_connect(impl->ssl.get());
        if (result == 1) {
            break;
        }
        const int error = SSL_get_error(impl->ssl.get(), result);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_ssl_ready(impl->descriptor, error, deadline)) {
            continue;
        }
        return nullptr;
    }
    return std::unique_ptr<TlsHttpConnection>(
        new TlsHttpConnection(std::move(impl)));
}

TlsHttpConnection::TlsHttpConnection(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TlsHttpConnection::~TlsHttpConnection() = default;

std::optional<network::Http1Response> TlsHttpConnection::request(
    std::string_view method,
    std::string_view target,
    const std::optional<std::string>& bearer,
    std::string_view body,
    std::chrono::steady_clock::time_point deadline) {
    std::string request;
    request.reserve(256 + body.size());
    request.append(method);
    request.push_back(' ');
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append("loadgen.realmmesh.example\r\n");
    if (bearer.has_value()) {
        request.append("Authorization: Bearer ");
        request.append(*bearer);
        request.append("\r\n");
    }
    if (!body.empty()) {
        request.append("Content-Type: application/json\r\n");
        request.append("Content-Length: ");
        request.append(std::to_string(body.size()));
        request.append("\r\n");
    }
    request.append("Connection: keep-alive\r\n\r\n");
    request.append(body);

    if (!impl_->write_all(
            reinterpret_cast<const unsigned char*>(request.data()),
            request.size(),
            deadline)) {
        return std::nullopt;
    }

    // 逐块收包直到解析出完整响应;解析器错误与对端断开一并视为
    // 网络层失败(连接作废,由调用方重建)。
    network::ByteBuffer buffer;
    for (;;) {
        auto parsed = impl_->parser.try_parse(buffer);
        if (parsed.status == network::Http1ResponseParseStatus::ResponseReady) {
            return std::move(parsed.response);
        }
        if (parsed.status !=
            network::Http1ResponseParseStatus::NeedMoreData) {
            return std::nullopt;
        }
        std::array<std::byte, 4096> chunk{};
        std::size_t received = 0;
        const int result = SSL_read_ex(
            impl_->ssl.get(), chunk.data(), chunk.size(), &received);
        if (result == 1) {
            buffer.append(
                std::span<const std::byte>(chunk.data(), received));
            continue;
        }
        const int error = SSL_get_error(impl_->ssl.get(), result);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_ssl_ready(impl_->descriptor, error, deadline)) {
            continue;
        }
        return std::nullopt;
    }
}

}  // namespace realm::loadgen
