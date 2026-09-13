#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/network/reactor/event_loop.hpp"

#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <thread>

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

TEST(TcpListenerTest, AcceptsANonBlockingLoopbackConnection) {
    using namespace std::chrono_literals;

    TcpListener listener("127.0.0.1", 0);
    auto event_loop = make_default_event_loop();
    ASSERT_NE(event_loop, nullptr);
    event_loop->add(to_event_loop_handle(listener.native_handle()), EventInterest::Read);

    EXPECT_NE(listener.local_port(), 0U);
    EXPECT_FALSE(listener.accept().has_value());

    const SocketGuard client(connect_to_loopback(listener.local_port()));
    ASSERT_FALSE(event_loop->wait(500ms).empty());
    auto accepted = listener.accept();

    ASSERT_TRUE(accepted.has_value());
    const int flags = ::fcntl(accepted->native_handle(), F_GETFL, 0);
    ASSERT_GE(flags, 0);
    EXPECT_NE(flags & O_NONBLOCK, 0);
}

TEST(TcpListenerTest, AcceptsIpv4ThroughAnIpv6DualStackListener) {
    using namespace std::chrono_literals;

    TcpListener listener("::", 0);
    auto event_loop = make_default_event_loop();
    ASSERT_NE(event_loop, nullptr);
    event_loop->add(to_event_loop_handle(listener.native_handle()), EventInterest::Read);

    const SocketGuard client(connect_to_loopback(listener.local_port()));
    ASSERT_FALSE(event_loop->wait(500ms).empty());
    EXPECT_TRUE(listener.accept().has_value());
}

/// 进程级兜底:TCP 平台后端在静态初始化阶段(早于 main)把 SIGPIPE 置为
/// 忽略,因此这里读回的"旧处置"必须是 SIG_IGN。这一层独立于
/// SO_NOSIGPIPE/MSG_NOSIGNAL,覆盖未经传输层创建的原生套接字。
///
/// 放在本文件是刻意的:静态库只在用到其符号时才被拉进二进制,本可执行文件
/// 因为用 TcpListener(→ create_stream_socket)才会链入平台后端 TU,这份初始化
/// 才会真的执行;单独拆一个文件反而需要人为制造引用。
TEST(TcpPlatformTest, IgnoresSigpipeProcessWide) {
    const auto previous = std::signal(SIGPIPE, SIG_IGN);
    ASSERT_NE(previous, SIG_ERR);
    EXPECT_EQ(previous, SIG_IGN);
}

/// 客户端在三次握手完成后、服务端 accept 之前就 RST 掉连接:内核仍会把这条
/// 夭折连接从完成队列交出来(macOS 上对它做的首组套接字选项立即 EINVAL,
/// Linux 则由 accept4 直接报 ECONNABORTED)。accept 路径必须把这类死连接
/// 静默丢弃,而不是掀翻调用方的事件循环——M3 规模冒烟下客户端 RST 很常见,
/// 该缺陷曾被定位为服务 tick 线程 terminate 的元凶。
TEST(TcpListenerTest, AcceptDropsConnectionResetBeforeAccept) {
    using namespace std::chrono_literals;

    TcpListener listener("127.0.0.1", 0);
    auto event_loop = make_default_event_loop();
    ASSERT_NE(event_loop, nullptr);
    event_loop->add(to_event_loop_handle(listener.native_handle()), EventInterest::Read);

    for (int round = 0; round < 8; ++round) {
        // 连接先落进完成队列,停一小拍再以 SO_LINGER{1,0} 发 RST,保证
        // accept 之前内核已经见过复位。
        const int client_descriptor = connect_to_loopback(listener.local_port());
        const SocketGuard client{client_descriptor};
        std::this_thread::sleep_for(20ms);
        const struct linger reset_close{1, 0};
        ASSERT_EQ(
            ::setsockopt(
                client_descriptor, SOL_SOCKET, SO_LINGER, &reset_close,
                sizeof(reset_close)),
            0);
        ASSERT_FALSE(event_loop->wait(500ms).empty());

        // 夭折连接要么被 accept 交出来(内部跳过),要么已被内核丢弃
        // (返回 nullopt);无论哪种,都不得抛异常。
        const auto accepted = listener.accept();
        if (accepted.has_value()) {
            const SocketGuard discarded{accepted->native_handle()};
        }
    }
}

}  // namespace
}  // namespace realm::network
