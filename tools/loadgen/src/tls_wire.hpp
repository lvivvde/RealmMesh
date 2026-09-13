#pragma once

// 两个 TLS 客户端(http 回包 / edge 帧)共用的拨号与就绪等待原语。
// 内部实现头:各客户端对外暴露的连接 API 与报文/帧语义各自持有,这里
// 只收敛 socket → TLS 握手 → 读写就绪这一段同型管道。
//
// 压测工具的关闭语义:socket 以 SO_LINGER{1,0} 建立,关闭即 RST 不进
// TIME_WAIT——万级机器人短连洪峰会把本机临时端口泡进 TIME_WAIT(macOS
// 默认段约 16k),端口耗尽后 dial 全线失败;响应/帧读完才关,无数据丢
// 失窗口(SSL_set_fd 为 BIO_NOCLOSE,fd 由 Stream 关闭,恰一次)。

#include "realmmesh/loadgen/stats.hpp"

#include <openssl/ssl.h>

#include <netdb.h>
#include <netinet/in.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace realm::loadgen::tls_wire {

struct SslContextDeleter {
    void operator()(SSL_CTX* value) const noexcept { SSL_CTX_free(value); }
};

struct SslDeleter {
    void operator()(SSL* value) const noexcept { SSL_free(value); }
};

[[nodiscard]] inline int remaining_ms(
    std::chrono::steady_clock::time_point deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    return remaining.count() <= 0 ? 0
                                  : static_cast<int>(remaining.count());
}

/// 非阻塞等待 SSL 想要的 IO 方向;超时返回 false。
[[nodiscard]] inline bool wait_ssl_ready(
    int descriptor,
    int ssl_error,
    std::chrono::steady_clock::time_point deadline) {
    const short events = ssl_error == SSL_ERROR_WANT_READ ? POLLIN : POLLOUT;
    std::array<::pollfd, 1> fds{{{descriptor, events, 0}}};
    const int timeout = remaining_ms(deadline);
    if (timeout <= 0) {
        return false;
    }
    return ::poll(fds.data(), 1, timeout) > 0;
}

/// 一条已握手的 TLS 流:socket 与 SSL 同生共死,析构即礼貌关闭。
struct Stream final {
    int descriptor{-1};
    std::unique_ptr<SSL_CTX, SslContextDeleter> context;
    std::unique_ptr<SSL, SslDeleter> ssl;

    ~Stream() {
        if (ssl != nullptr) {
            static_cast<void>(SSL_shutdown(ssl.get()));
        }
        if (descriptor >= 0) {
            static_cast<void>(::close(descriptor));
        }
    }
};

/// 拨号 + TLS 握手(alpn_name 为单个应用协议名,如 "http/1.1"),全程
/// 受截止约束;失败返回 false(失败路径自清理,调用方直接复用 out)。
[[nodiscard]] inline bool dial_stream(
    const ServiceAddress& address,
    std::string_view alpn_name,
    std::chrono::steady_clock::time_point deadline,
    Stream& out) {
    ::addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    ::addrinfo* resolved = nullptr;
    if (::getaddrinfo(
            address.host.c_str(),
            std::to_string(address.port).c_str(),
            &hints,
            &resolved) != 0 ||
        resolved == nullptr) {
        return false;
    }
    const int descriptor = ::socket(
        resolved->ai_family, resolved->ai_socktype, resolved->ai_protocol);
    if (descriptor < 0) {
        ::freeaddrinfo(resolved);
        return false;
    }
    // 从这里起 fd 归 out 所有:任何失败路径直接 return,由 Stream 析构
    // 按 SSL_shutdown → close 的次序收尾,不得提前 close(否则析构里的
    // SSL_shutdown 会写向已释放的描述符)。
    out.descriptor = descriptor;
    // 压测工具的关闭语义:SO_LINGER 0 = 关闭即 RST,不进 TIME_WAIT。
    // 万级机器人 × 每机器人数条连接的短连洪峰会把本机临时端口泡进
    // TIME_WAIT(macOS 默认段约 16k),端口耗尽后 dial 全线失败;响应
    // 读完才关,无数据丢失窗口。
    const struct linger reset_close{1, 0};
    static_cast<void>(::setsockopt(
        descriptor, SOL_SOCKET, SO_LINGER, &reset_close,
        sizeof(reset_close)));
    // 非阻塞拨号:连接与握手全程受截止约束,坏地址不挂死机器人。
    const int flags = ::fcntl(descriptor, F_GETFL, 0);
    if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) {
        ::freeaddrinfo(resolved);
        return false;
    }
    const int connected =
        ::connect(descriptor, resolved->ai_addr, resolved->ai_addrlen);
    ::freeaddrinfo(resolved);
    if (connected != 0) {
        if (errno != EINPROGRESS) {
            return false;
        }
        std::array<::pollfd, 1> fds{{{descriptor, POLLOUT, 0}}};
        const int timeout = remaining_ms(deadline);
        if (timeout <= 0 || ::poll(fds.data(), 1, timeout) != 1) {
            return false;
        }
        int socket_error = 0;
        socklen_t error_size = sizeof(socket_error);
        if (::getsockopt(
                descriptor, SOL_SOCKET, SO_ERROR, &socket_error,
                &error_size) != 0 ||
            socket_error != 0) {
            return false;
        }
    }

    out.context.reset(SSL_CTX_new(TLS_client_method()));
    // 压测对象是自签证书环境(测试/soak 档):不校验服务端证书,应用层
    // 凭据(token)承担身份;诊断工具不收私有输入。
    SSL_CTX_set_verify(out.context.get(), SSL_VERIFY_NONE, nullptr);
    out.ssl.reset(SSL_new(out.context.get()));
    // 服务端握手要求协商出 ALPN(http/1.1 或 realmmesh-edge/1),不带即
    // 被回绝关闭;单协议的 wire 形式 = 长度前缀 + 名字。
    std::string alpn_wire(1, static_cast<char>(alpn_name.size()));
    alpn_wire += alpn_name;
    if (out.ssl == nullptr ||
        SSL_set_fd(out.ssl.get(), descriptor) != 1 ||
        SSL_set_tlsext_host_name(out.ssl.get(), address.host.c_str()) != 1 ||
        SSL_set_alpn_protos(
            out.ssl.get(),
            reinterpret_cast<const unsigned char*>(alpn_wire.data()),
            alpn_wire.size()) != 0) {
        return false;
    }
    for (;;) {
        const int result = SSL_connect(out.ssl.get());
        if (result == 1) {
            break;
        }
        const int error = SSL_get_error(out.ssl.get(), result);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_ssl_ready(descriptor, error, deadline)) {
            continue;
        }
        return false;
    }
    return true;
}

/// 非阻塞写完全部字节;失败返回 false。
[[nodiscard]] inline bool write_all(
    Stream& stream,
    const unsigned char* data,
    std::size_t size,
    std::chrono::steady_clock::time_point deadline) {
    std::size_t offset = 0;
    while (offset < size) {
        std::size_t written = 0;
        if (SSL_write_ex(
                stream.ssl.get(), data + offset, size - offset, &written) ==
            1) {
            offset += written;
            continue;
        }
        const int error = SSL_get_error(stream.ssl.get(), 0);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            wait_ssl_ready(stream.descriptor, error, deadline)) {
            continue;
        }
        return false;
    }
    return true;
}

}  // namespace realm::loadgen::tls_wire
