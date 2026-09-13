#pragma once

// TLS 客户端流(TLS/TCP 载体,ADR-0007 客户端侧):非阻塞 socket +
// OpenSSL 握手,全部 IO 受截止时间约束;停止令牌在等待循环里逐片检查,
// 让竞速中落败的一路能尽快收手。
//
// 关闭语义:默认优雅关闭(SSL_shutdown → close)。万级短连的压测场景
// 需要关闭即 RST(不进 TIME_WAIT),以 reset_close_on_release 显式开启。

#include "realmmesh/network/client/secure_byte_stream.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace realm::network::client {

/// 拨号失败分型:调用方按型处置(重试/退避/换候选/记诊断)。
enum class TlsDialFailure {
    None,
    Resolve,
    Socket,
    Configure,
    Connect,
    ConnectTimeout,
    SslSetup,
    Handshake,
    Cancelled,
};

[[nodiscard]] std::string_view tls_dial_failure_name(
    TlsDialFailure failure) noexcept;

struct TlsClientOptions final {
    /// 提议的单个 ALPN 协议名(如 "http/1.1";空 = 不提议)。
    std::string alpn;
    /// 校验服务端证书。生产默认校验;自签证书环境显式关闭。
    bool verify_peer{true};
    /// 释放即 RST(压测万级短连语义);产品默认优雅关闭。
    bool reset_close_on_release{false};
};

/// 拨号结果(字节流面 + 分型):HTTP/edge 会话直接消费,竞速胜出者
/// 也能以同一形状交给会话层。
struct ClientDialResult final {
    std::shared_ptr<ISecureByteStream> stream;
    TlsDialFailure failure{TlsDialFailure::None};
    int last_errno{0};
    long verify_result{0};

    [[nodiscard]] bool ok() const noexcept { return stream != nullptr; }
};

/// 一条已握手的 TLS 客户端流:socket 与 SSL 同生共死。
class TlsClientStream final : public ISecureConnection,
                              public ISecureByteStream {
public:
    struct DialResult final {
        std::shared_ptr<TlsClientStream> stream;
        TlsDialFailure failure{TlsDialFailure::None};
        /// Connect/ConnectTimeout 路径最近一次的 errno(诊断线索)。
        int last_errno{0};
        /// 握手失败时的证书校验结果(X509_V_OK 为 0;诊断线索)。
        long verify_result{0};

        [[nodiscard]] bool ok() const noexcept { return stream != nullptr; }
    };

    /// 拨号 + TLS 握手;失败路径自清理,结果里带分型。
    [[nodiscard]] static DialResult dial(
        std::string_view host,
        std::uint16_t port,
        const TlsClientOptions& options,
        StreamDeadline deadline,
        std::stop_token stop = {});

    ~TlsClientStream() override;
    TlsClientStream(const TlsClientStream&) = delete;
    TlsClientStream& operator=(const TlsClientStream&) = delete;
    TlsClientStream(TlsClientStream&&) = delete;
    TlsClientStream& operator=(TlsClientStream&&) = delete;

    [[nodiscard]] TransportProtocol protocol() const noexcept override;
    [[nodiscard]] ISecureByteStream* stream() noexcept override;
    /// 协商出的 ALPN 协议名(未协商为空)。
    [[nodiscard]] const std::string& negotiated_alpn() const noexcept;

    [[nodiscard]] bool write_all(
        std::span<const std::byte> data,
        StreamDeadline deadline) override;
    [[nodiscard]] std::optional<std::size_t> read_some(
        std::span<std::byte> out,
        StreamDeadline deadline) override;
    void shutdown() override;

private:
    struct Impl;
    explicit TlsClientStream(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::network::client
