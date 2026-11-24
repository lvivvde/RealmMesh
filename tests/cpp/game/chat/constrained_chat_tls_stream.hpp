#pragma once

// C5 测试私有 mTLS 客户端：握手前收紧 TCP 接收窗口，业务停止读取后
// 让 Chat 的生产 1MiB 出站缓冲因真实积压达到上限。
#include "realmmesh/network/client/secure_byte_stream.hpp"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#ifdef __linux__
#include <netinet/tcp.h>
#endif
#include <unistd.h>
#include <algorithm>
#include <cerrno>
#include <memory>
#include <stdexcept>
#include <string>

namespace realm::game::chat::test {
class ConstrainedChatTlsStream final
    : public network::client::ISecureByteStream {
    struct Socket {
        int fd{-1};
        ~Socket() {
            if (fd >= 0) ::close(fd);
        }
    } socket_;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context_{
        nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl_{nullptr, SSL_free};
    bool closed_{false};

    bool wait(
        int error, network::client::StreamDeadline deadline) const noexcept {
        if (error != SSL_ERROR_WANT_READ && error != SSL_ERROR_WANT_WRITE)
            return false;
        while (network::client::StreamClock::now() < deadline) {
            const auto left =
                std::chrono::duration_cast<std::chrono::milliseconds>(
                    deadline - network::client::StreamClock::now())
                    .count();
            pollfd descriptor{
                socket_.fd,
                static_cast<short>(
                    error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT),
                0};
            const auto ready = ::poll(
                &descriptor,
                1,
                static_cast<int>(std::min<decltype(left)>(left, 1000)));
            if (ready > 0) return true;
            if (ready < 0 && errno != EINTR) return false;
        }
        return false;
    }

public:
    ConstrainedChatTlsStream(
        std::uint16_t port,
        std::string_view ca_file,
        std::string_view certificate_file,
        std::string_view private_key_file,
        std::string_view server_identity,
        network::client::StreamDeadline deadline) {
        socket_.fd = ::socket(AF_INET, SOCK_STREAM, 0);
        const int receive_bytes = 1024;
        if (socket_.fd < 0 ||
            ::setsockopt(
                socket_.fd,
                SOL_SOCKET,
                SO_RCVBUF,
                &receive_bytes,
                sizeof(receive_bytes)) != 0 ||
            ::fcntl(socket_.fd, F_SETFL, O_NONBLOCK) != 0)
            throw std::runtime_error("constrained Chat socket setup failed");
#ifdef __linux__
        // SO_RCVBUF 在 connect 前关闭 Linux 的接收窗口自动调优；再收紧窗口
        // clamp，避免快 runner 在慢端停止读取后继续通告扩大后的窗口。
        if (::setsockopt(
                socket_.fd,
                IPPROTO_TCP,
                TCP_WINDOW_CLAMP,
                &receive_bytes,
                sizeof(receive_bytes)) != 0)
            throw std::runtime_error("constrained Chat receive window setup failed");
#endif
#ifdef SO_NOSIGPIPE
        const int enabled = 1;
        if (::setsockopt(
                socket_.fd,
                SOL_SOCKET,
                SO_NOSIGPIPE,
                &enabled,
                sizeof(enabled)) != 0)
            throw std::runtime_error("constrained Chat SIGPIPE setup failed");
#endif
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        address.sin_port = htons(port);
        if (::connect(
                socket_.fd,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) != 0 &&
            (errno != EINPROGRESS || !wait(SSL_ERROR_WANT_WRITE, deadline)))
            throw std::runtime_error("constrained Chat connect failed");

        const std::string ca(ca_file), certificate(certificate_file),
            private_key(private_key_file), identity(server_identity);
        context_.reset(SSL_CTX_new(TLS_client_method()));
        if (!context_ ||
            SSL_CTX_load_verify_locations(context_.get(), ca.c_str(), nullptr) !=
                1 ||
            SSL_CTX_use_certificate_chain_file(
                context_.get(), certificate.c_str()) != 1 ||
            SSL_CTX_use_PrivateKey_file(
                context_.get(), private_key.c_str(), SSL_FILETYPE_PEM) != 1)
            throw std::runtime_error("constrained Chat TLS context failed");
        SSL_CTX_set_verify(context_.get(), SSL_VERIFY_PEER, nullptr);
        ssl_.reset(SSL_new(context_.get()));
        static constexpr unsigned char alpn[] = {
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
            'c',
            'h',
            'a',
            't',
            '/',
            '1'};
        if (!ssl_ || SSL_set_fd(ssl_.get(), socket_.fd) != 1 ||
            SSL_set_tlsext_host_name(ssl_.get(), identity.c_str()) != 1 ||
            X509_VERIFY_PARAM_set1_host(
                SSL_get0_param(ssl_.get()), identity.c_str(), 0) != 1 ||
            SSL_set_alpn_protos(ssl_.get(), alpn, sizeof(alpn)) != 0)
            throw std::runtime_error("constrained Chat TLS setup failed");
        for (;;) {
            ERR_clear_error();
            const auto result = SSL_connect(ssl_.get());
            if (result == 1) break;
            if (!wait(SSL_get_error(ssl_.get(), result), deadline))
                throw std::runtime_error(
                    "constrained Chat TLS handshake failed");
        }
    }

    bool write_all(
        std::span<const std::byte> data,
        network::client::StreamDeadline deadline) override {
        if (closed_) return false;
        while (!data.empty()) {
            std::size_t size = 0;
            ERR_clear_error();
            const auto result =
                SSL_write_ex(ssl_.get(), data.data(), data.size(), &size);
            if (result == 1)
                data = data.subspan(size);
            else if (!wait(SSL_get_error(ssl_.get(), result), deadline))
                return false;
        }
        return true;
    }

    std::optional<std::size_t> read_some(
        std::span<std::byte> output,
        network::client::StreamDeadline deadline) override {
        if (closed_) return 0;
        for (;;) {
            std::size_t size = 0;
            ERR_clear_error();
            const auto result =
                SSL_read_ex(ssl_.get(), output.data(), output.size(), &size);
            if (result == 1) return size;
            const auto error = SSL_get_error(ssl_.get(), result);
            if (error == SSL_ERROR_ZERO_RETURN) return 0;
            if (!wait(error, deadline)) return std::nullopt;
        }
    }

    void shutdown() override {
        if (!closed_) {
            ::shutdown(socket_.fd, SHUT_RDWR);
            closed_ = true;
        }
    }
};
}  // namespace realm::game::chat::test
