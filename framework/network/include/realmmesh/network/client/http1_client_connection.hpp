#pragma once

// 最小 HTTP/1.1 客户端连接(ADR-0007 客户端边):在一条 TLS/TCP 流上
// 逐请求一响应,保活复用;所有读写受截止时间约束。拨号失败分型沿用
// TlsDialFailure(dial 直接透出)。
//
// 只服务本仓服务契约:v1 无 chunked(解析器一律拒绝 Transfer-Encoding),
// 无 body 的请求不发 Content-Length,响应体为空不读。

#include "realmmesh/network/client/tls_client_stream.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"
#include "realmmesh/network/http/http1_response.hpp"
#include "realmmesh/network/http/http1_response_parser.hpp"

#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>

namespace realm::network::client {

class Http1ClientConnection final {
public:
    /// 拨号 + TLS 握手(ALPN http/1.1,options.alpn 给出时以其为准)。
    [[nodiscard]] static ClientDialResult dial(
        std::string_view host,
        std::uint16_t port,
        const TlsClientOptions& options,
        StreamDeadline deadline,
        std::stop_token stop = {});

    /// 复用已建立的流(如竞速胜出的连接);流须已按 http/1.1 协商 ALPN。
    explicit Http1ClientConnection(std::shared_ptr<ISecureByteStream> stream);
    ~Http1ClientConnection();
    Http1ClientConnection(const Http1ClientConnection&) = delete;
    Http1ClientConnection& operator=(const Http1ClientConnection&) = delete;

    /// 一次请求-响应;网络层失败或响应畸形返回 nullopt(连接作废)。
    /// host_header 只填 Host 头,不参与路由。
    [[nodiscard]] std::optional<network::Http1Response> request(
        std::string_view method,
        std::string_view target,
        std::string_view host_header,
        const std::optional<std::string>& bearer,
        std::string_view body,
        StreamDeadline deadline);

private:
    std::shared_ptr<ISecureByteStream> stream_;
    network::Http1ResponseParser parser_;
    /// 解析器消费剩余(保活复用时上一次多读的字节)。
    network::ByteBuffer pending_;
};

}  // namespace realm::network::client
