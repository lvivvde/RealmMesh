#include "tcp_platform.hpp"

#include <cerrno>
#include <fcntl.h>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace realm::network::detail {
namespace {

// 进程级兜底,语义见 tcp_platform.hpp;静态初始化在 main 之前执行。
[[maybe_unused]] const IgnoreSigpipeOnStartup ignore_sigpipe_on_startup{};

// macOS 没有 MSG_NOSIGNAL,无法在每次 send 时声明"不产生 SIGPIPE";改为在
// 套接字上置 SO_NOSIGPIPE。OpenSSL 最终写的是同一个 fd,因此同样受保护,
// 即使上面的进程级兜底被替换掉,TLS/TCP 传输也不会被 SIGPIPE 打掉。
void disable_sigpipe_on_socket(int descriptor) {
    const int enabled = 1;
    if (::setsockopt(
            descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) <
        0) {
        throw std::system_error(
            errno, std::generic_category(), "setsockopt(SO_NOSIGPIPE)");
    }
}

// macOS 的 socket() 不接受 SOCK_NONBLOCK / SOCK_CLOEXEC,也没有 accept4,
// 只能在创建后补 fcntl;accept() 同样不继承 close-on-exec。
void make_nonblocking_and_cloexec(int descriptor) {
    const int status_flags = ::fcntl(descriptor, F_GETFL, 0);
    if (status_flags < 0 ||
        ::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) < 0) {
        throw std::system_error(
            errno, std::generic_category(), "fcntl(F_SETFL)");
    }

    const int descriptor_flags = ::fcntl(descriptor, F_GETFD, 0);
    if (descriptor_flags < 0 ||
        ::fcntl(descriptor, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0) {
        throw std::system_error(
            errno, std::generic_category(), "fcntl(F_SETFD)");
    }
}

}  // namespace

int create_stream_socket(bool ipv6) {
    const int descriptor = ::socket(ipv6 ? AF_INET6 : AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        throw std::system_error(errno, std::generic_category(), "socket");
    }

    try {
        make_nonblocking_and_cloexec(descriptor);
        disable_sigpipe_on_socket(descriptor);
    } catch (...) {
        ::close(descriptor);
        throw;
    }
    return descriptor;
}

int accept_nonblocking(int listener) {
    while (true) {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client >= 0) {
            // 对端可能在 accept 返回前后的一瞬发来 RST:macOS 对已断开的
            // TCP 套接字做 setsockopt 直接 EINVAL。刚接回来就死掉的连接
            // 只能跳过,不能掀翻调用方的事件循环(实测一万取号并发下
            // 客户端 RST 会稳定命中这个窗口)。
            try {
                make_nonblocking_and_cloexec(client);
                disable_sigpipe_on_socket(client);
            } catch (const std::system_error& error) {
                ::close(client);
                if (error.code() == std::errc::invalid_argument) {
                    continue;
                }
                throw;
            }
            return client;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == ECONNABORTED) {
            // 完成队列里的连接已被对端中止,等同无连接,继续收下一个。
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return -1;
        }
        throw std::system_error(errno, std::generic_category(), "accept");
    }
}

}  // namespace realm::network::detail
