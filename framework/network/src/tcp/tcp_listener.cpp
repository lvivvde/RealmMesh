#include "realmmesh/network/tcp/tcp_listener.hpp"

#include "tcp_platform.hpp"

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
    const bool ipv6 = address.find(':') != std::string_view::npos;
    const int descriptor = detail::create_stream_socket(ipv6);

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

    if (ipv6) {
        const int dual_stack = 0;
        if (::setsockopt(
                descriptor_,
                IPPROTO_IPV6,
                IPV6_V6ONLY,
                &dual_stack,
                sizeof(dual_stack)) < 0) {
            const int error = errno;
            close();
            throw std::system_error(
                error, std::generic_category(), "setsockopt(IPV6_V6ONLY)");
        }
    }

    sockaddr_storage socket_address{};
    socklen_t socket_address_size = 0;

    const std::string address_text(address);
    int parse_result = 0;
    if (ipv6) {
        auto& value = reinterpret_cast<sockaddr_in6&>(socket_address);
        value.sin6_family = AF_INET6;
        value.sin6_port = htons(port);
        parse_result = ::inet_pton(
            AF_INET6, address_text.c_str(), &value.sin6_addr);
        socket_address_size = sizeof(value);
    } else {
        auto& value = reinterpret_cast<sockaddr_in&>(socket_address);
        value.sin_family = AF_INET;
        value.sin_port = htons(port);
        parse_result = ::inet_pton(
            AF_INET, address_text.c_str(), &value.sin_addr);
        socket_address_size = sizeof(value);
    }
    if (parse_result != 1) {
        close();
        throw std::invalid_argument("invalid IP listen address");
    }

    if (::bind(
            descriptor_,
            reinterpret_cast<const sockaddr*>(&socket_address),
            socket_address_size) < 0) {
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

    local_port_ = ipv6
                      ? ntohs(reinterpret_cast<sockaddr_in6&>(socket_address).sin6_port)
                      : ntohs(reinterpret_cast<sockaddr_in&>(socket_address).sin_port);
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
    const int client = detail::accept_nonblocking(descriptor_);
    if (client < 0) {
        return std::nullopt;
    }
    return TcpSocket(client);
}

void TcpListener::close() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    local_port_ = 0;
}

}  // namespace realm::network
