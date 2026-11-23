#pragma once

// S5 测试私有 TLS 客户端：握手前收紧 TCP 接收窗口，业务停止读取后
// 让真实服务端出站缓冲达到上限。证书只用于本机生成的测试身份。
#include "realmmesh/network/client/secure_byte_stream.hpp"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <memory>
#include <stdexcept>

namespace realm::test_support {
class ConstrainedTlsStream final : public network::client::ISecureByteStream {
    struct Socket {
        int fd{-1};
        ~Socket() { if (fd >= 0) ::close(fd); }
    } socket_;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_{nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_{nullptr, SSL_free};
    bool closed_{false};
    bool wait(int error, network::client::StreamDeadline deadline) {
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
            return false;
        for (;;) {
            const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - network::client::StreamClock::now()).count();
            if (left <= 0) return false;
            pollfd descriptor{socket_.fd, static_cast<short>(
                error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT), 0};
            const auto ready = ::poll(&descriptor, 1, static_cast<int>(std::min<decltype(left)>(left, 1000)));
            if (ready > 0) return true;
            if (ready < 0 && errno != EINTR) return false;
        }
    }
public:
    ConstrainedTlsStream(std::uint16_t port, network::client::StreamDeadline deadline) {
        socket_.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int receive_bytes = 1024;
        if (socket_.fd < 0 ||
            ::setsockopt(socket_.fd, SOL_SOCKET, SO_RCVBUF, &receive_bytes, sizeof(receive_bytes)) != 0 ||
            ::fcntl(socket_.fd, F_SETFL, O_NONBLOCK) != 0)
            throw std::runtime_error("constrained TLS socket setup failed");
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if (::setsockopt(socket_.fd, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
            throw std::runtime_error("constrained TLS SIGPIPE setup failed");
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(socket_.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 &&
            (errno != EINPROGRESS || !wait(SSL_ERROR_WANT_WRITE, deadline)))
            throw std::runtime_error("constrained TLS connect failed");
        context_.reset(SSL_CTX_new(TLS_client_method()));
        if (!context_) throw std::runtime_error("constrained TLS context failed");
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_NONE, nullptr);
        ssl_.reset(SSL_new(context_.get()));
        const unsigned char alpn[] = {16, 'r','e','a','l','m','m','e','s','h','-','e','d','g','e','/','1'};
        if (!ssl_ || SSL_set_fd(ssl_.get(), socket_.fd) != 1 ||
            SSL_set_alpn_protos(ssl_.get(), alpn, sizeof(alpn)) != 0)
            throw std::runtime_error("constrained TLS initialization failed");
        for (;;) {
            ERR_clear_error();
            const auto result = SSL_connect(ssl_.get());
            if (result == 1) break;
            if (!wait(SSL_get_error(ssl_.get(), result), deadline))
                throw std::runtime_error("constrained TLS handshake failed");
        }
    }
    bool write_all(std::span<const std::byte> data, network::client::StreamDeadline deadline) override {
        if (closed_) return false;
        while (!data.empty()) {
            std::size_t size = 0;
            ERR_clear_error();
            const auto result = SSL_write_ex(ssl_.get(), data.data(), data.size(), &size);
            if (result == 1) data = data.subspan(size);
            else if (!wait(SSL_get_error(ssl_.get(), result), deadline)) return false;
        }
        return true;
    }
    std::optional<std::size_t> read_some(std::span<std::byte> out, network::client::StreamDeadline deadline) override {
        if (closed_) return 0;
        for (;;) {
            std::size_t size = 0;
            ERR_clear_error();
            const auto result = SSL_read_ex(ssl_.get(), out.data(), out.size(), &size);
            if (result == 1) return size;
            const auto error = SSL_get_error(ssl_.get(), result);
            if (error == SSL_ERROR_ZERO_RETURN) return 0;
            if (!wait(error, deadline)) return std::nullopt;
        }
    }
    void shutdown() override {
        if (!closed_) { ::shutdown(socket_.fd, SHUT_RDWR); closed_ = true; }
    }
};
}  // namespace realm::test_support
