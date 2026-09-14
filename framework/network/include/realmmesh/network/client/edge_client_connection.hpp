#pragma once

// 网关 edge 传输的最小 TLS 客户端(ALPN realmmesh-edge/1,长度前缀帧):
// 发一帧、收一帧,协议编解码复用 game/common 的信封(本类只管帧边界)。
// 既可直接拨号(测试/工具),也可复用竞速胜出的流(客户端链路)。

#include "realmmesh/network/client/tls_client_stream.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace realm::network::client {

/// 单帧负载上限:attach 与 1303 授权(票据 + 端点)在 1KB 内,
/// 上限放宽到 16KB 只为容忍端点列表扩展。
inline constexpr std::size_t kMaxEdgeFramePayload = 16 * 1024;

class EdgeClientConnection final {
public:
    /// 拨号 + TLS 握手(ALPN realmmesh-edge/1;options.alpn 给出时以其为准)。
    [[nodiscard]] static ClientDialResult dial(
        std::string_view host,
        std::uint16_t port,
        const TlsClientOptions& options,
        StreamDeadline deadline,
        std::stop_token stop = {});

    /// 复用已建立的流(竞速胜出的连接);流须已按 realmmesh-edge/1 协商。
    explicit EdgeClientConnection(std::shared_ptr<ISecureByteStream> stream);
    /// 借用流:不接管所有权,调用方保证流在对象生命周期内存活。兑换口在
    /// 调用期内复用链路竞速出的 Realm 流,正是这种用法。
    explicit EdgeClientConnection(ISecureByteStream& stream);
    ~EdgeClientConnection();
    EdgeClientConnection(const EdgeClientConnection&) = delete;
    EdgeClientConnection& operator=(const EdgeClientConnection&) = delete;

    /// 发送一帧(payload 为信封字节,长度前缀由编码器补齐)。
    [[nodiscard]] bool send_frame(std::span<const std::byte> payload,
                                  StreamDeadline deadline);

    /// 收一帧(信封字节);超时、对端断开或帧超限返回 nullopt。
    [[nodiscard]] std::optional<std::vector<std::byte>> receive_frame(
        StreamDeadline deadline);

private:
    /// 持有流的情形(自己拨号或接管);借用构造时为空。
    std::shared_ptr<ISecureByteStream> owned_;
    ISecureByteStream* stream_{};
    network::LengthFieldCodec codec_{kMaxEdgeFramePayload};
    /// 已收到但尚未成帧的字节(帧跨读边界时留存)。
    std::vector<std::byte> pending_;
};

}  // namespace realm::network::client
