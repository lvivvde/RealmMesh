#pragma once

#include "realmmesh/network/core/byte_buffer.hpp"
#include "realmmesh/network/http/http1_parser.hpp"
#include "realmmesh/network/http/http1_response.hpp"
#include "realmmesh/network/reactor/event_loop.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/network/tls/tls_connection.hpp"
#include "realmmesh/network/tls/tls_server_context.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace realm::network {

struct HttpServerConfig final {
    /// 证书链 + 私钥;HTTP 监听建议配 ALPN = http/1.1。
    TransportConfig::TlsServerIdentity tls_identity;
    std::size_t max_head_bytes{Http1Parser::default_max_head_bytes};
    std::size_t max_body_bytes{Http1Parser::default_max_body_bytes};
    std::size_t max_pending_output_bytes{64 * 1024};
    std::size_t max_connections{1024};
    /// 解析中(不完整请求)截止——防慢速发送占用连接(截止自请求首字节起算,
    /// 不随后续字节延长)。
    std::chrono::milliseconds parse_timeout{15000};
    /// 完整请求间(keep-alive)截止。
    std::chrono::milliseconds idle_timeout{60000};
};

/// 完整请求到达后同步回调(在 loop 线程执行,不得阻塞)。
using HttpHandler = std::function<Http1Response(const Http1Request&)>;

/// 自研最小 HTTPS 服务边(ADR-0007):TcpListener + IEventLoop +
/// TlsServerContext + 流模式 TlsConnection + 有界 HTTP/1.1 解析。
/// 轮询模型:属主线程驱动 poll_once,与消息传输同一形状;单 loop,
/// 洪峰分片 = 多 runtime 实例(规格 §5.3)。协议层错误(400/413/431/505)
/// 由服务边直接回绝并关闭连接,不进 handler。
class HttpServer final {
public:
    /// port 传 0 时由内核分配,local_port() 取实际值。
    HttpServer(
        std::string_view address,
        std::uint16_t port,
        HttpServerConfig config,
        HttpHandler handler);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] std::uint16_t local_port() const noexcept;

    /// 驱动一轮:accept → 读写 → 解析 → handler → 回写,含截止清扫。
    /// timeout 是无事件时的最长阻塞;有连接临近截止时取更短者。
    void poll_once(std::chrono::milliseconds timeout);

private:
    struct Connection {
        TlsConnection connection;
        Http1Parser parser;
        ByteBuffer input;
        std::chrono::steady_clock::time_point last_activity;
        std::chrono::steady_clock::time_point parse_started_at;
        bool handshake_complete{false};
        bool close_after_flush{false};
        bool parse_in_progress{false};
        TlsIoState io_need{TlsIoState::WantRead};
    };

    void accept_connections();
    void service_connection(Connection& connection, const ReadyEvent& ready);
    [[nodiscard]] bool dispatch_request(
        Connection& connection,
        Http1Request request);
    void reject_and_close(Connection& connection, Http1ParseStatus status);
    void close_connection(Connection& connection);
    void sweep_deadlines();
    void update_interest(Connection& connection);
    [[nodiscard]] std::chrono::steady_clock::time_point connection_deadline(
        const Connection& connection) const;
    [[nodiscard]] std::chrono::milliseconds time_to_earliest_deadline()
        const;

    HttpServerConfig config_;
    HttpHandler handler_;
    TlsServerContext tls_context_;
    TcpListener listener_;
    std::unique_ptr<IEventLoop> event_loop_;
    std::map<EventLoopHandle, Connection> connections_;
};

}  // namespace realm::network
