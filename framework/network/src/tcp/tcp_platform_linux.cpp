#include "tcp_platform.hpp"

#include <cerrno>
#include <csignal>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace realm::network::detail {
namespace {

// 兜底:进程启动阶段忽略 SIGPIPE,写入已关闭/被 RST 的对端时不会被内核
// 终止,而是让 write 返回 EPIPE 交由传输层处理。
//
// Linux 的 per-send 方案是 MSG_NOSIGNAL,但本仓库的 TCP 写入经 OpenSSL
// (SSL_write)发出,无法逐次带上该标志;SIGPIPE 是进程级信号,忽略一次即
// 覆盖所有套接字,故以此作为 Linux 侧的保护。静态初始化在 main 之前执行,
// 且 TCP 平台后端只编译一个,所以每个进程至多安装一次。
struct IgnoreSigpipeOnStartup {
    IgnoreSigpipeOnStartup() noexcept {
        static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    }
};
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
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        throw std::system_error(errno, std::generic_category(), "accept4");
    }
}

}  // namespace realm::network::detail
