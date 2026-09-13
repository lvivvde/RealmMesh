#pragma once

#include "realmmesh/loadgen/stats.hpp"
#include "realmmesh/network/http/http1_response.hpp"

#include <chrono>
#include <memory>
#include <optional>
#include <string>

namespace realm::loadgen {

/// 阻塞语义、非阻塞实现的最小 TLS HTTP/1.1 客户端(loadgen 自足,
/// ADR-0007 范围内:产品化测试侧客户端先例,SSL 直连)。拨号、握手、
/// 读写一律受截止时间约束;连接保活可复用(逐请求一响应)。
class TlsHttpConnection final {
public:
    /// 拨号 + TLS 握手(ALPN http/1.1);失败返回 nullptr。
    static std::unique_ptr<TlsHttpConnection> dial(
        const ServiceAddress& address,
        std::chrono::steady_clock::time_point deadline);

    ~TlsHttpConnection();
    TlsHttpConnection(const TlsHttpConnection&) = delete;
    TlsHttpConnection& operator=(const TlsHttpConnection&) = delete;

    /// 一次请求-响应;网络层失败或对端断开返回 nullopt(连接作废)。
    /// body 为空时不发 Content-Length(无 body 请求)。
    [[nodiscard]] std::optional<network::Http1Response> request(
        std::string_view method,
        std::string_view target,
        const std::optional<std::string>& bearer,
        std::string_view body,
        std::chrono::steady_clock::time_point deadline);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    explicit TlsHttpConnection(std::unique_ptr<Impl> impl);
};

}  // namespace realm::loadgen
