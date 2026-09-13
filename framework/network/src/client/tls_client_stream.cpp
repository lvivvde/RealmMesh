#include "realmmesh/network/client/tls_client_stream.hpp"

#include <openssl/ssl.h>

#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>

namespace realm::network::client {
namespace {

constexpr std::chrono::milliseconds kStopCheckSlice{50};

[[nodiscard]] int remaining_ms(StreamDeadline deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - StreamClock::now());
    if (remaining.count() <= 0) {
        return 0;
    }
    if (remaining.count() > static_cast<std::int64_t>(
                               std::numeric_limits<int>::max())) {
        return std::numeric_limits<int>::max();
    }
    return static_cast<int>(remaining.count());
}

/// 等 SSL/socket 就绪;带停止令牌时按 kStopCheckSlice 分片以便响应取消。
[[nodiscard]] bool wait_ready(int descriptor,
                              short events,
                              StreamDeadline deadline,
                              const std::stop_token* stop) {
    for (;;) {
        if (stop != nullptr && stop->stop_requested()) {
            return false;
        }
        const int left = remaining_ms(deadline);
        if (left <= 0) {
            return false;
        }
        const int slice =
            stop == nullptr ? left : std::min(left,
                                              static_cast<int>(
                                                  kStopCheckSlice.count()));
        std::array<::pollfd, 1> fds{{{descriptor, events, 0}}};
        const int waited = ::poll(fds.data(), 1, slice);
        if (waited > 0) {
            return true;
        }
        if (waited < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        // 超时:无停止令牌即整体超时;有令牌时是分片边界,回环重判。
        if (stop == nullptr) {
            return false;
        }
    }
}

[[nodiscard]] short events_for_ssl_error(int ssl_error) {
    return ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
}

struct SslContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};

struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

}  // namespace

std::string_view tls_dial_failure_name(TlsDialFailure failure) noexcept {
    switch (failure) {
        case TlsDialFailure::None:
            return "none";
        case TlsDialFailure::Resolve:
            return "resolve";
        case TlsDialFailure::Socket:
            return "socket";
        case TlsDialFailure::Configure:
            return "configure";
        case TlsDialFailure::Connect:
            return "connect";
        case TlsDialFailure::ConnectTimeout:
            return "connect_timeout";
        case TlsDialFailure::SslSetup:
            return "ssl_setup";
        case TlsDialFailure::Handshake:
            return "handshake";
        case TlsDialFailure::Cancelled:
            return "cancelled";
    }
    return "unknown";
}

struct TlsClientStream::Impl final {
    int descriptor{-1};
    std::unique_ptr<SSL_CTX, SslContextDeleter> context;
    std::unique_ptr<SSL, SslDeleter> ssl;
    std::string negotiated_alpn;
    bool closed{false};

    ~Impl() {
        if (!closed) {
            close_now();
        }
    }

    void close_now() noexcept {
        closed = true;
        if (ssl != nullptr) {
            static_cast<void>(SSL_shutdown(ssl.get()));
            ssl.reset();
        }
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
            descriptor = -1;
        }
    }
};

TlsClientStream::TlsClientStream(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TlsClientStream::~TlsClientStream() = default;

TransportProtocol TlsClientStream::protocol() const noexcept {
    return TransportProtocol::TlsTcp;
}

ISecureByteStream* TlsClientStream::stream() noexcept {
    return this;
}

const std::string& TlsClientStream::negotiated_alpn() const noexcept {
    return impl_->negotiated_alpn;
}

TlsClientStream::DialResult TlsClientStream::dial(
    std::string_view host,
    std::uint16_t port,
    const TlsClientOptions& options,
    StreamDeadline deadline,
    std::stop_token stop) {
    DialResult result;
    if (stop.stop_requested()) {
        result.failure = TlsDialFailure::Cancelled;
        return result;
    }

    ::addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo* resolved = nullptr;
    const std::string port_text = std::to_string(port);
    if (::getaddrinfo(
            std::string(host).c_str(), port_text.c_str(), &hints, &resolved) !=
            0 ||
        resolved == nullptr) {
        result.failure = TlsDialFailure::Resolve;
        return result;
    }

    auto impl = std::make_unique<Impl>();
    const int descriptor = ::socket(
        resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (descriptor < 0) {
        result.last_errno = errno;
        result.failure = TlsDialFailure::Socket;
        ::freeaddrinfo(resolved);
        return result;
    }
    // fd 从此刻起归 impl:失败路径直接返回,由析构按 SSL_shutdown →
    // close 的次序收尾(不得提前 close,否则析构会写向已释放的描述符)。
    impl->descriptor = descriptor;
    if (options.reset_close_on_release) {
        const struct linger reset_close{1, 0};
        static_cast<void>(::setsockopt(
            descriptor, SOL_SOCKET, SO_LINGER, &reset_close,
            sizeof(reset_close)));
    }
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        result.last_errno = errno;
        result.failure = TlsDialFailure::Configure;
        ::freeaddrinfo(resolved);
        return result;
    }

    const int connected =
        ::connect(descriptor, resolved->ai_addr, resolved->ai_addrlen);
    ::freeaddrinfo(resolved);
    if (connected != 0) {
        if (errno != EINPROGRESS) {
            result.last_errno = errno;
            result.failure = TlsDialFailure::Connect;
            return result;
        }
        const std::stop_token* stop_ptr = stop.stop_possible() ? &stop : nullptr;
        if (!wait_ready(descriptor, POLLOUT, deadline, stop_ptr)) {
            result.last_errno = errno;
            result.failure = (stop_ptr != nullptr && stop.stop_requested())
                                 ? TlsDialFailure::Cancelled
                                 : TlsDialFailure::ConnectTimeout;
            return result;
        }
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        if (::getsockopt(
                descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                &error_size) != 0 ||
            socket_error != 0) {
            result.last_errno = socket_error != 0 ? socket_error : errno;
            result.failure = TlsDialFailure::Connect;
            return result;
        }
    }

    impl->context.reset(SSL_CTX_new(TLS_client_method()));
    SSL_CTX_set_verify(
        impl->context.get(),
        options.verify_peer ? SSL_VERIFY_PEER : SSL_VERIFY_NONE,
        nullptr);
    impl->ssl.reset(SSL_new(impl->context.get()));
    const std::string host_text{host};
    const std::string alpn_wire =
        options.alpn.empty()
            ? std::string{}
            : std::string(1, static_cast<char>(options.alpn.size())) +
                  options.alpn;
    if (impl->ssl == nullptr ||
        SSL_set_fd(impl->ssl.get(), descriptor) != 1 ||
        SSL_set_tlsext_host_name(impl->ssl.get(), host_text.c_str()) != 1 ||
        (!alpn_wire.empty() &&
         SSL_set_alpn_protos(
             impl->ssl.get(),
             reinterpret_cast<const unsigned char*>(alpn_wire.data()),
             static_cast<unsigned int>(alpn_wire.size())) != 0)) {
        result.failure = TlsDialFailure::SslSetup;
        return result;
    }

    for (;;) {
        if (stop.stop_possible() && stop.stop_requested()) {
            result.failure = TlsDialFailure::Cancelled;
            return result;
        }
        const int handshake = SSL_connect(impl->ssl.get());
        if (handshake == 1) {
            break;
        }
        const int error = SSL_get_error(impl->ssl.get(), handshake);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_ready(
                descriptor, events_for_ssl_error(error), deadline,
                stop.stop_possible() ? &stop : nullptr)) {
            continue;
        }
        if (stop.stop_possible() && stop.stop_requested()) {
            result.failure = TlsDialFailure::Cancelled;
            return result;
        }
        result.verify_result = SSL_get_verify_result(impl->ssl.get());
        result.failure = TlsDialFailure::Handshake;
        return result;
    }

    const unsigned char* selected = nullptr;
    unsigned int selected_size = 0;
    SSL_get0_alpn_selected(impl->ssl.get(), &selected, &selected_size);
    if (selected != nullptr && selected_size > 0) {
        impl->negotiated_alpn.assign(
            reinterpret_cast<const char*>(selected), selected_size);
    }

    result.stream = std::shared_ptr<TlsClientStream>(
        new TlsClientStream(std::move(impl)));
    return result;
}

bool TlsClientStream::write_all(std::span<const std::byte> data,
                                StreamDeadline deadline) {
    std::size_t offset = 0;
    while (offset < data.size()) {
        std::size_t written = 0;
        if (SSL_write_ex(
                impl_->ssl.get(),
                data.data() + static_cast<std::ptrdiff_t>(offset),
                data.size() - offset, &written) == 1) {
            offset += written;
            continue;
        }
        const int error = SSL_get_error(impl_->ssl.get(), 0);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_ready(impl_->descriptor, events_for_ssl_error(error),
                       deadline, nullptr)) {
            continue;
        }
        return false;
    }
    return true;
}

std::optional<std::size_t> TlsClientStream::read_some(
    std::span<std::byte> out,
    StreamDeadline deadline) {
    for (;;) {
        std::size_t received = 0;
        const int result =
            SSL_read_ex(impl_->ssl.get(), out.data(), out.size(), &received);
        if (result == 1) {
            return received;
        }
        const int error = SSL_get_error(impl_->ssl.get(), result);
        if (error == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_ready(impl_->descriptor, events_for_ssl_error(error),
                       deadline, nullptr)) {
            continue;
        }
        return std::nullopt;
    }
}

void TlsClientStream::shutdown() {
    impl_->close_now();
}

}  // namespace realm::network::client
