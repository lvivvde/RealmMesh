#pragma once

namespace realm::network::detail {

/// 创建流式套接字并置为非阻塞、close-on-exec;失败抛 std::system_error。
[[nodiscard]] int create_stream_socket(bool ipv6);

/// 接受一个连接并置为非阻塞。无待处理连接时返回 -1(不抛异常);
/// EINTR 在实现内部重试,其它失败抛 std::system_error。
[[nodiscard]] int accept_nonblocking(int listener);

}  // namespace realm::network::detail
