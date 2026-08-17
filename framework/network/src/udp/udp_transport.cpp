#include "realmmesh/network/udp/udp_transport.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
#include <utility>

namespace realm::network {
namespace {

[[noreturn]] void throw_socket_error(const char* operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}

}  // namespace

UdpTransport::UdpTransport(TransportConfig config) : config_(std::move(config)) {
    descriptor_ = ::socket(
        AF_INET,
        SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0);
    if (descriptor_ < 0) {
        throw_socket_error("socket(udp)");
    }

    try {
        const int reuse_address = 1;
        if (::setsockopt(
                descriptor_,
                SOL_SOCKET,
                SO_REUSEADDR,
                &reuse_address,
                sizeof(reuse_address)) < 0) {
            throw_socket_error("setsockopt(SO_REUSEADDR)");
        }

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(config_.listen_port);
        if (::inet_pton(
                AF_INET,
                config_.listen_address.c_str(),
                &address.sin_addr) != 1) {
            throw std::invalid_argument("invalid IPv4 UDP listen address");
        }
        if (::bind(
                descriptor_,
                reinterpret_cast<const sockaddr*>(&address),
                sizeof(address)) < 0) {
            throw_socket_error("bind(udp)");
        }

        socklen_t address_size = sizeof(address);
        if (::getsockname(
                descriptor_,
                reinterpret_cast<sockaddr*>(&address),
                &address_size) < 0) {
            throw_socket_error("getsockname(udp)");
        }
        local_port_ = ntohs(address.sin_port);
        event_loop_.add(descriptor_, EventInterest::Read);
    } catch (...) {
        close_socket();
        throw;
    }
}

UdpTransport::~UdpTransport() {
    close_socket();
}

std::string_view UdpTransport::name() const noexcept {
    return config_.name;
}

TransportProtocol UdpTransport::protocol() const noexcept {
    return TransportProtocol::Udp;
}

TransportEndpoint UdpTransport::local_endpoint() const {
    return {
        .name = config_.name,
        .protocol = TransportProtocol::Udp,
        .address = config_.listen_address,
        .port = local_port_,
    };
}

std::size_t UdpTransport::session_count() const noexcept {
    return peers_.size();
}

std::vector<TransportEvent> UdpTransport::poll_once(
    std::chrono::milliseconds timeout) {
    std::vector<TransportEvent> events;
    const auto ready_events = event_loop_.wait(timeout);
    if (ready_events.empty() || !ready_events.front().readable) {
        return events;
    }

    std::vector<std::byte> buffer(config_.max_payload_size + 1);
    while (true) {
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const auto received = ::recvfrom(
            descriptor_,
            buffer.data(),
            buffer.size(),
            0,
            reinterpret_cast<sockaddr*>(&peer),
            &peer_size);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                break;
            }
            throw_socket_error("recvfrom(udp)");
        }

        const auto payload_size = static_cast<std::size_t>(received);
        if (payload_size > config_.max_payload_size) {
            continue;
        }

        const auto key = peer_key(peer);
        auto session_iterator = sessions_by_peer_.find(key);
        SessionId session_id = invalid_session_id;
        if (session_iterator == sessions_by_peer_.end()) {
            if (peers_.size() >= config_.max_sessions) {
                continue;
            }
            session_id = next_session_id_++;
            sessions_by_peer_.emplace(key, session_id);
            peers_.emplace(session_id, PeerAddress{peer});
            events.push_back({
                .kind = TransportEventKind::SessionOpened,
                .session_id = session_id,
                .payload = {},
            });
        } else {
            session_id = session_iterator->second;
        }

        events.push_back({
            .kind = TransportEventKind::MessageReceived,
            .session_id = session_id,
            .payload = std::vector<std::byte>(
                buffer.begin(),
                buffer.begin() + static_cast<std::ptrdiff_t>(payload_size)),
        });
    }
    return events;
}

bool UdpTransport::send(
    SessionId session_id,
    std::span<const std::byte> payload) {
    const auto iterator = peers_.find(session_id);
    if (iterator == peers_.end() || payload.size() > config_.max_payload_size) {
        return false;
    }

    while (true) {
        const auto sent = ::sendto(
            descriptor_,
            payload.data(),
            payload.size(),
            MSG_NOSIGNAL,
            reinterpret_cast<const sockaddr*>(&iterator->second.address),
            sizeof(iterator->second.address));
        if (sent >= 0) {
            return static_cast<std::size_t>(sent) == payload.size();
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return false;
        }
        throw_socket_error("sendto(udp)");
    }
}

bool UdpTransport::close(SessionId session_id) {
    const auto iterator = peers_.find(session_id);
    if (iterator == peers_.end()) {
        return false;
    }
    sessions_by_peer_.erase(peer_key(iterator->second.address));
    peers_.erase(iterator);
    return true;
}

std::string UdpTransport::peer_key(const sockaddr_in& address) {
    std::string key(sizeof(address.sin_addr.s_addr) + sizeof(address.sin_port), '\0');
    std::memcpy(key.data(), &address.sin_addr.s_addr, sizeof(address.sin_addr.s_addr));
    std::memcpy(
        key.data() + sizeof(address.sin_addr.s_addr),
        &address.sin_port,
        sizeof(address.sin_port));
    return key;
}

void UdpTransport::close_socket() noexcept {
    if (descriptor_ >= 0) {
        ::close(descriptor_);
        descriptor_ = -1;
    }
    local_port_ = 0;
}

}  // namespace realm::network
