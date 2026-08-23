#include "realmmesh/network/tcp/tcp_socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

namespace realm::network {

TcpPeerEndpoint TcpSocket::peer_endpoint() const noexcept {
    TcpPeerEndpoint endpoint;
    if (descriptor_ < 0) {
        return endpoint;
    }
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getpeername(
            descriptor_, reinterpret_cast<sockaddr*>(&storage), &length) != 0) {
        return endpoint;
    }
    char buffer[INET6_ADDRSTRLEN]{};
    if (storage.ss_family == AF_INET) {
        const auto& address =
            reinterpret_cast<const sockaddr_in&>(storage).sin_addr;
        if (::inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) != nullptr) {
            endpoint.host = buffer;
        }
        endpoint.port =
            ntohs(reinterpret_cast<const sockaddr_in&>(storage).sin_port);
    } else if (storage.ss_family == AF_INET6) {
        const auto& address =
            reinterpret_cast<const sockaddr_in6&>(storage).sin6_addr;
        if (::inet_ntop(AF_INET6, &address, buffer, sizeof(buffer)) !=
            nullptr) {
            endpoint.host = buffer;
        }
        endpoint.port =
            ntohs(reinterpret_cast<const sockaddr_in6&>(storage).sin6_port);
    }
    return endpoint;
}

}  // namespace realm::network
