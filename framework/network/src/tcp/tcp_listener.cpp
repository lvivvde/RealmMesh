#include "realmmesh/network/tcp/tcp_listener.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <string>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace realm::network {
namespace {

[[noreturn]] void throw_socket_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

}  // namespace

TcpSocket::TcpSocket(int descriptor) noexcept : descriptor_(descriptor) {}

TcpSocket::~TcpSocket() {
    close();
}

TcpSocket::TcpSocket(TcpSocket&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)) {}

TcpSocket& TcpSocket::operator=(TcpSocket&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
}

int TcpSocket::native_handle() const noexcept {
    return descriptor_;
}

bool TcpSocket::valid() const noexcept {
    return descriptor_ >= 0;
}

void TcpSocket::close() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
}

TcpListener::TcpListener(std::string_view address, std::uint16_t port, int backlog) {
    const int descriptor = ::socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0);
    if (descriptor < 0) {
        throw_socket_error("socket");
    }

    descriptor_ = descriptor;

    const int reuse_address = 1;
    if (::setsockopt(
            descriptor_,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuse_address,
            sizeof(reuse_address)) < 0) {
        const int error = errno;
        close();
        throw std::system_error(error, std::generic_category(), "setsockopt(SO_REUSEADDR)");
    }

    sockaddr_in socket_address{};
    socket_address.sin_family = AF_INET;
    socket_address.sin_port = htons(port);

    const std::string address_text(address);
    const int parse_result = ::inet_pton(
        AF_INET,
        address_text.c_str(),
        &socket_address.sin_addr);
    if (parse_result != 1) {
        close();
        throw std::invalid_argument("invalid IPv4 listen address");
    }

    if (::bind(
            descriptor_,
            reinterpret_cast<const sockaddr*>(&socket_address),
            sizeof(socket_address)) < 0) {
        const int error = errno;
        close();
        throw std::system_error(error, std::generic_category(), "bind");
    }

    if (::listen(descriptor_, backlog) < 0) {
        const int error = errno;
        close();
        throw std::system_error(error, std::generic_category(), "listen");
    }

    socklen_t address_size = sizeof(socket_address);
    if (::getsockname(
            descriptor_,
            reinterpret_cast<sockaddr*>(&socket_address),
            &address_size) < 0) {
        const int error = errno;
        close();
        throw std::system_error(error, std::generic_category(), "getsockname");
    }

    local_port_ = ntohs(socket_address.sin_port);
}

TcpListener::~TcpListener() {
    close();
}

TcpListener::TcpListener(TcpListener&& other) noexcept
    : descriptor_(std::exchange(other.descriptor_, -1)),
      local_port_(std::exchange(other.local_port_, 0)) {}

TcpListener& TcpListener::operator=(TcpListener&& other) noexcept {
    if (this != &other) {
        close();
        descriptor_ = std::exchange(other.descriptor_, -1);
        local_port_ = std::exchange(other.local_port_, 0);
    }
    return *this;
}

int TcpListener::native_handle() const noexcept {
    return descriptor_;
}

std::uint16_t TcpListener::local_port() const noexcept {
    return local_port_;
}

std::optional<TcpSocket> TcpListener::accept() {
    while (true) {
        const int client = ::accept4(
            descriptor_,
            nullptr,
            nullptr,
            SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client >= 0) {
            return TcpSocket(client);
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return std::nullopt;
        }
        throw_socket_error("accept4");
    }
}

void TcpListener::close() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    local_port_ = 0;
}

}  // namespace realm::network
