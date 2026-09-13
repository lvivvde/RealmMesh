#pragma once

#include "realmmesh/loadgen/stats.hpp"

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace realm::loadgen {

/// 网关 edge 传输的最小 TLS 客户端(ALPN realmmesh-edge/1,长度前缀
/// 帧):发一帧、收一帧。attach → 1302 受理 → 1303 授权链路的客户端
/// 侧,协议编解码复用 game/common。
class TlsEdgeConnection final {
public:
    /// 拨号 + TLS 握手;失败返回 nullptr。
    static std::unique_ptr<TlsEdgeConnection> dial(
        const ServiceAddress& address,
        std::chrono::steady_clock::time_point deadline);

    ~TlsEdgeConnection();
    TlsEdgeConnection(const TlsEdgeConnection&) = delete;
    TlsEdgeConnection& operator=(const TlsEdgeConnection&) = delete;

    /// 发送一帧(payload 为信封字节,长度前缀由编解码器补齐)。
    [[nodiscard]] bool send_frame(
        std::span<const std::byte> payload,
        std::chrono::steady_clock::time_point deadline);

    /// 收一帧(信封字节);超时或对端断开返回 nullopt。
    [[nodiscard]] std::optional<std::vector<std::byte>> receive_frame(
        std::chrono::steady_clock::time_point deadline);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit TlsEdgeConnection(std::unique_ptr<Impl> impl);
};

}  // namespace realm::loadgen
