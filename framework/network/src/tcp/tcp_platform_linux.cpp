#include "tcp_platform.hpp"

#include <cerrno>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace realm::network::detail {

int create_stream_socket(bool ipv6) {
    const int descriptor = ::socket(
        ipv6 ? AF_INET6 : AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }
    return descriptor;
}

int accept_nonblocking(int listener) {
    while (true) {
        const int client = ::accept4(
            listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client >= 0) {
            return client;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        throw std::system_error(errno, std::generic_category(), "accept4");
    }
}

}  // namespace realm::network::detail
