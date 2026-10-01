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
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>

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

/// 挂起响应的凭据(#98,ADR-0007 补记):进程内单调递增,不复用。
using HttpResponseToken = std::uint64_t;
/// handler 返回它表示挂起:慢工作(如 Argon2 验签)交给 loop 外的有界
/// 工作者,结果回到属主线程后以 HttpServer::complete 补发。
struct HttpDeferred final {};
using HttpHandlerResult = std::variant<Http1Response, HttpDeferred>;
/// 可挂起的 handler:同样在 loop 线程执行、不得阻塞;token 只在返回
/// HttpDeferred 时有意义。
using HttpDeferringHandler =
    std::function<HttpHandlerResult(const Http1Request&, HttpResponseToken)>;

/// 自研最小 HTTPS 服务边(ADR-0007):TcpListener + IEventLoop +
/// TlsServerContext + 流模式 TlsConnection + 有界 HTTP/1.1 解析。
/// 轮询模型:属主线程驱动 poll_once,与消息传输同一形状;单 loop,
/// 洪峰分片 = 多 runtime 实例(规格 §5.3)。协议层错误(400/413/431/505)
/// 由服务边直接回绝并关闭连接,不进 handler。
///
/// 挂起响应:handler 挂起期间该连接不再解析后续(流水线)请求,已收字节
/// 留在缓冲里,补发后按序继续;挂起期间截止按 idle_timeout 自请求完整时
/// 起算;对端半关闭(close_notify)不放弃该响应,补发写完后再关闭连接。
class HttpServer final {
public:
    /// port 传 0 时由内核分配,local_port() 取实际值。
    HttpServer(
        std::string_view address,
        std::uint16_t port,
        HttpServerConfig config,
        HttpHandler handler);
    HttpServer(
        std::string_view address,
        std::uint16_t port,
        HttpServerConfig config,
        HttpDeferringHandler handler);
    ~HttpServer();

    HttpServer(const HttpServer&) = delete;
    HttpServer& operator=(const HttpServer&) = delete;

    [[nodiscard]] std::uint16_t local_port() const noexcept;

    /// 驱动一轮:accept → 读写 → 解析 → handler → 回写,含截止清扫。
    /// timeout 是无事件时的最长阻塞;有连接临近截止时取更短者。
    void poll_once(std::chrono::milliseconds timeout);

    /// 补发挂起的响应;须在属主线程、poll_once 之外调用。连接已关闭
    /// (截止、连接出错、背压)或 token 未知时返回 false,响应丢弃。
    bool complete(HttpResponseToken token, Http1Response response);

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
        /// 挂起中的响应;有值时暂停解析。
        std::optional<HttpResponseToken> awaiting;
        bool awaiting_keep_alive{true};
        /// 对端已关闭写方向(close_notify):不再读,应答写完即关。
        bool peer_closed{false};
        /// 是否挂在 event loop 上;半关闭后等挂起响应时摘下。
        bool registered{true};
        TlsIoState io_need{TlsIoState::WantRead};
    };

    void accept_connections();
    void service_connection(Connection& connection, const ReadyEvent& ready);
    /// 两者返回 false 表示连接已关闭(connection 已析构)。
    [[nodiscard]] bool parse_and_dispatch(Connection& connection);
    [[nodiscard]] bool dispatch_request(
        Connection& connection,
        Http1Request request);
    [[nodiscard]] bool queue_response(
        Connection& connection,
        const Http1Response& response,
        bool keep_alive);
    void flush_and_settle(Connection& connection, bool writable);
    void reject_and_close(Connection& connection, Http1ParseStatus status);
    void close_connection(Connection& connection);
    void sweep_deadlines();
    void update_interest(Connection& connection);
    [[nodiscard]] std::chrono::steady_clock::time_point connection_deadline(
        const Connection& connection) const;
    [[nodiscard]] std::chrono::milliseconds time_to_earliest_deadline()
        const;

    HttpServerConfig config_;
    HttpDeferringHandler handler_;
    TlsServerContext tls_context_;
    TcpListener listener_;
    std::unique_ptr<IEventLoop> event_loop_;
    std::map<EventLoopHandle, Connection> connections_;
    HttpResponseToken next_token_{1};
    std::unordered_map<HttpResponseToken, EventLoopHandle> awaiting_;
};

}  // namespace realm::network
