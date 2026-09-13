#include "tcp_platform.hpp"

#include <cerrno>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace realm::network::detail {
namespace {

// 进程级兜底(Linux 侧的唯一一层),语义见 tcp_platform.hpp:MSG_NOSIGNAL
// 无法穿透 SSL_write,故在启动阶段整体忽略 SIGPIPE。
[[maybe_unused]] const IgnoreSigpipeOnStartup ignore_sigpipe_on_startup{};

}  // namespace

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
        if (errno == ECONNABORTED) {
            // accept(2):已挂起连接上的网络错误会经 accept 吐出;对端在
            // accept 前已中止的连接等同无连接,继续收下一个,不能掀翻
            // 调用方的事件循环。
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        throw std::system_error(errno, std::generic_category(), "accept4");
    }
}

}  // namespace realm::network::detail
