#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/network/reactor/epoll_event_loop.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

namespace realm::network {
namespace {

class SocketGuard final {
public:
    explicit SocketGuard(int descriptor)
        : descriptor_(descriptor) {}
    ~SocketGuard() {
        if (descriptor_ >= 0) {
            ::close(descriptor_);
        }
    }

    SocketGuard(const SocketGuard&) = delete;
    SocketGuard& operator=(const SocketGuard&) = delete;

private:
    int descriptor_;
};

int connect_to_loopback(std::uint16_t port) {
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
        throw std::runtime_error("failed to create test client socket");
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (::connect(
            descriptor,
            reinterpret_cast<const sockaddr*>(&address),
            sizeof(address)) < 0) {
        ::close(descriptor);
        throw std::runtime_error("failed to connect test client socket");
    }

    return descriptor;
}

TEST(TcpListenerTest, AcceptsANonBlockingLoopbackConnection) {
    using namespace std::chrono_literals;

    TcpListener listener("127.0.0.1", 0);
    EpollEventLoop event_loop;
    event_loop.add(listener.native_handle(), EventInterest::Read);

    EXPECT_NE(listener.local_port(), 0U);
    EXPECT_FALSE(listener.accept().has_value());

    const SocketGuard client(connect_to_loopback(listener.local_port()));
    ASSERT_FALSE(event_loop.wait(500ms).empty());
    auto accepted = listener.accept();

    ASSERT_TRUE(accepted.has_value());
    const int flags = ::fcntl(accepted->native_handle(), F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE(flags & O_NONBLOCK, 0);
}

TEST(EpollEventLoopTest, ReportsListenerAsReadableWhenClientConnects) {
    using namespace std::chrono_literals;

    TcpListener listener("127.0.0.1", 0);
    EpollEventLoop event_loop;
    event_loop.add(listener.native_handle(), EventInterest::Read);

    const SocketGuard client(connect_to_loopback(listener.local_port()));
    const auto events = event_loop.wait(500ms);

    const auto event =
        std::ranges::find_if(events, [&listener](const ReadyEvent& value) {
            return value.descriptor == listener.native_handle();
        });
    ASSERT_NE(event, events.end());
    EXPECT_TRUE(event->readable);
    EXPECT_FALSE(event->writable);

    EXPECT_TRUE(listener.accept().has_value());
}

TEST(TcpListenerTest, AcceptsIpv4ThroughAnIpv6DualStackListener) {
    using namespace std::chrono_literals;

    TcpListener listener("::", 0);
    EpollEventLoop event_loop;
    event_loop.add(listener.native_handle(), EventInterest::Read);

    const SocketGuard client(connect_to_loopback(listener.local_port()));
    ASSERT_FALSE(event_loop.wait(500ms).empty());
    EXPECT_TRUE(listener.accept().has_value());
}

}  // namespace
}  // namespace realm::network
