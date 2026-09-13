#pragma once

#include <csignal>

namespace realm::network::detail {

/// 启动时忽略 SIGPIPE,作为 TCP 平台后端的进程级兜底。
///
/// 写入已关闭/被 RST 的对端时,内核默认以 SIGPIPE 终止进程(而不是让 write
/// 返回 EPIPE)。本仓库的写入经 OpenSSL(SSL_write)发出,Linux 的 per-send
/// MSG_NOSIGNAL 无法逐次带上,SIGPIPE 又是进程级信号,所以两个平台后端都在
/// 静态初始化阶段(早于 main)忽略一次;macOS 另在套接字上置 SO_NOSIGPIPE。
///
/// 该类型本身不含平台差异,故放共享头;各后端 TU 自行实例化一份(平台后端只
/// 编译一个,每个进程至多安装一次)。注意静态库的拉取语义:只有在后端 TU 被
/// 链接进二进制(即用到 create_stream_socket 等符号)时,这份初始化才会执行。
struct IgnoreSigpipeOnStartup {
    IgnoreSigpipeOnStartup() noexcept {
        static_cast<void>(std::signal(SIGPIPE, SIG_IGN));
    }
};

/// 创建流式套接字并置为非阻塞、close-on-exec;失败抛 std::system_error。
[[nodiscard]] int create_stream_socket(bool ipv6);

/// 接受一个连接并置为非阻塞。无待处理连接时返回 -1(不抛异常);EINTR 在
/// 实现内部重试;对端在 accept 前后一瞬已 RST 的夭折连接被静默丢弃
/// (Linux 经 accept 报 ECONNABORTED,macOS 上表现为首组套接字选项
/// EINVAL),其余失败抛 std::system_error。
[[nodiscard]] int accept_nonblocking(int listener);

}  // namespace realm::network::detail
