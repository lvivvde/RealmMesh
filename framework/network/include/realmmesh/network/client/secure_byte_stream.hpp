#pragma once

// 加密连接的两个面(ADR-0007 客户端侧):
// - ISecureConnection 是竞速结果的最小契约:只承诺"握手完成 + 用了哪个传输";
// - ISecureByteStream 是流式载体的可选字节面(TLS/TCP),消息型载体
//   (QUIC MessageTransport)不提供,故 stream() 默认返回 nullptr。
// 分开声明是为了让竞速(PreferredTransportConnector)不依赖字节面,
// 而 HTTP/1.1 与 edge 帧客户端可以只依赖字节面。

#include "realmmesh/network/transport/message_transport.hpp"

#include <chrono>
#include <cstddef>
#include <optional>
#include <span>

namespace realm::network::client {

using StreamClock = std::chrono::steady_clock;
using StreamDeadline = StreamClock::time_point;

/// 已建立加密连接的字节流面;所有 IO 阻塞语义、非阻塞实现,受截止约束。
class ISecureByteStream {
public:
    virtual ~ISecureByteStream() = default;

    /// 写完全部字节;失败或超时返回 false。
    [[nodiscard]] virtual bool write_all(
        std::span<const std::byte> data,
        StreamDeadline deadline) = 0;

    /// 读一段;0 字节 = 对端已关闭;失败或超时返回 nullopt。
    [[nodiscard]] virtual std::optional<std::size_t> read_some(
        std::span<std::byte> out,
        StreamDeadline deadline) = 0;

    /// 终止本次会话(关闭底层载体;幂等)。
    virtual void shutdown() = 0;
};

class ISecureConnection {
public:
    virtual ~ISecureConnection() = default;
    [[nodiscard]] virtual TransportProtocol protocol() const noexcept = 0;

    /// 该连接支持字节流读写时返回之,否则 nullptr(消息型载体)。
    [[nodiscard]] virtual ISecureByteStream* stream() noexcept {
        return nullptr;
    }
};

}  // namespace realm::network::client
