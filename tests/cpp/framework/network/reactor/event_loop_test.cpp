#include "realmmesh/network/reactor/event_loop.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
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
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
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

EventLoopHandle listener_handle(const TcpListener& listener) {
    return to_event_loop_handle(listener.native_handle());
}

bool reports(
    const std::vector<ReadyEvent>& events,
    EventLoopHandle handle,
    bool ReadyEvent::* flag) {
    const auto event =
        std::ranges::find_if(events, [handle](const ReadyEvent& value) {
            return value.handle == handle;
        });
    return event != events.end() && (*event).*flag;
}

// Runs against whichever backend the platform selects (ADR-0001), so kqueue is
// held to the same contract as epoll once that backend lands.
TEST(EventLoopTest, CreatesAPlatformBackend) {
    EXPECT_NE(make_default_event_loop(), nullptr);
}

TEST(EventLoopTest, ReportsListenerAsReadableWhenClientConnects) {
    using namespace std::chrono_literals;

    auto event_loop = make_default_event_loop();
    ASSERT_NE(event_loop, nullptr);

    TcpListener listener("127.0.0.1", 0);
    const auto handle = listener_handle(listener);
    event_loop->add(handle, EventInterest::Read);

    const SocketGuard client(connect_to_loopback(listener.local_port()));
    const auto events = event_loop->wait(500ms);

    EXPECT_TRUE(reports(events, handle, &ReadyEvent::readable));
    EXPECT_FALSE(reports(events, handle, &ReadyEvent::writable));
    EXPECT_TRUE(listener.accept().has_value());
}

TEST(EventLoopTest, StopsReportingAfterRemove) {
    using namespace std::chrono_literals;

    auto event_loop = make_default_event_loop();
    ASSERT_NE(event_loop, nullptr);

    TcpListener listener("127.0.0.1", 0);
    const auto handle = listener_handle(listener);
    event_loop->add(handle, EventInterest::Read);
    event_loop->remove(handle);

    const SocketGuard client(connect_to_loopback(listener.local_port()));
    EXPECT_FALSE(reports(event_loop->wait(100ms), handle, &ReadyEvent::readable));
    EXPECT_TRUE(listener.accept().has_value());
}

TEST(EventLoopTest, ReportsWritableOnlyWhileWriteInterestIsRegistered) {
    using namespace std::chrono_literals;

    auto event_loop = make_default_event_loop();
    ASSERT_NE(event_loop, nullptr);

    TcpListener listener("127.0.0.1", 0);
    event_loop->add(listener_handle(listener), EventInterest::Read);
    const SocketGuard client(connect_to_loopback(listener.local_port()));
    ASSERT_FALSE(event_loop->wait(500ms).empty());

    auto accepted = listener.accept();
    ASSERT_TRUE(accepted.has_value());
    const auto handle = to_event_loop_handle(accepted->native_handle());

    event_loop->add(handle, EventInterest::Read | EventInterest::Write);
    EXPECT_TRUE(
        reports(event_loop->wait(500ms), handle, &ReadyEvent::writable));

    event_loop->modify(handle, EventInterest::Read);
    EXPECT_FALSE(
        reports(event_loop->wait(100ms), handle, &ReadyEvent::writable));
}

}  // namespace
}  // namespace realm::network
