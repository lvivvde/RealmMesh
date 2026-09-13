#include "realmmesh/loadgen/edge_client.hpp"

#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"

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
#include <span>
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

/// 单帧负载上限:attach 与 1303 授权(票据 + 端点)在 1KB 内,
/// 上限放宽到 16KB 只为容忍端点列表扩展。
constexpr std::size_t kMaxFramePayload = 16 * 1024;

[[nodiscard]] int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    return remaining.count() <= 0 ? 0 : static_cast<int>(remaining.count());
}

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

struct TlsEdgeConnection::Impl final {
    int descriptor{-1};
    std::unique_ptr<SSL_CTX, SslContextDeleter> context;
    std::unique_ptr<SSL, SslDeleter> ssl;
    network::LengthFieldCodec codec{kMaxFramePayload};
    std::vector<std::byte> pending;

    ~Impl() {
        if (ssl != nullptr) {
            static_cast<void>(SSL_shutdown(ssl.get()));
        }
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
    }
};

std::unique_ptr<TlsEdgeConnection> TlsEdgeConnection::dial(
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
    // 压测工具的关闭语义:SO_LINGER 0 = 关闭即 RST,不进 TIME_WAIT
    // (同 http_client,响应读完才关,无数据丢失窗口)。
    const struct linger reset_close{1, 0};
    static_cast<void>(::setsockopt(
        impl->descriptor,
        SOL_SOCKET,
        SO_LINGER,
        &reset_close,
        sizeof(reset_close)));
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
    // 自签证书环境(与 HTTP 客户端同策):应用层凭据承担身份。
    SSL_CTX_set_verify(impl->context.get(), SSL_VERIFY_NONE, nullptr);
    impl->ssl.reset(SSL_new(impl->context.get()));
    static constexpr std::array<unsigned char, 17> alpn{
        16, 'r', 'e', 'a', 'l', 'm', 'm', 'e', 's',
        'h', '-', 'e', 'd', 'g', 'e', '/', '1'};
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
    return std::unique_ptr<TlsEdgeConnection>(
        new TlsEdgeConnection(std::move(impl)));
}

TlsEdgeConnection::TlsEdgeConnection(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TlsEdgeConnection::~TlsEdgeConnection() = default;

bool TlsEdgeConnection::send_frame(
    std::span<const std::byte> payload,
    std::chrono::steady_clock::time_point deadline) {
    const auto framed = impl_->codec.encode(payload);
    std::size_t offset = 0;
    while (offset < framed.size()) {
        std::size_t written = 0;
        if (SSL_write_ex(
                impl_->ssl.get(),
                framed.data() + offset,
                framed.size() - offset,
                &written) == 1) {
            offset += written;
            continue;
        }
        const int error = SSL_get_error(impl_->ssl.get(), 0);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_ssl_ready(impl_->descriptor, error, deadline)) {
            continue;
        }
        return false;
    }
    return true;
}

std::optional<std::vector<std::byte>> TlsEdgeConnection::receive_frame(
    std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        network::ByteBuffer buffer;
        buffer.append(impl_->pending);
        const auto decoded = impl_->codec.try_decode(buffer);
        if (decoded.status == network::DecodeStatus::FrameReady) {
            const auto consumed = impl_->pending.size() - buffer.readable_bytes();
            impl_->pending.erase(
                impl_->pending.begin(),
                impl_->pending.begin() +
                    static_cast<std::ptrdiff_t>(consumed));
            return decoded.payload;
        }
        if (decoded.status == network::DecodeStatus::FrameTooLarge) {
            return std::nullopt;
        }
        std::array<std::byte, 4096> chunk{};
        std::size_t received = 0;
        const int result = SSL_read_ex(
            impl_->ssl.get(), chunk.data(), chunk.size(), &received);
        if (result == 1) {
            impl_->pending.insert(
                impl_->pending.end(),
                chunk.begin(),
                chunk.begin() + static_cast<std::ptrdiff_t>(received));
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
