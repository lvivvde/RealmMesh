#include "realmmesh/network/http/http_server.hpp"

#include <algorithm>
#include <utility>
#include <vector>

namespace realm::network {

namespace {

/// 协议层错误 → 状态码(规格:错误响应后必须关闭连接)。
int status_for(Http1ParseStatus parse_status) {
    switch (parse_status) {
    case Http1ParseStatus::PayloadTooLarge: return 413;
    case Http1ParseStatus::HeadersTooLarge: return 431;
    case Http1ParseStatus::VersionNotSupported: return 505;
    default: return 400;
    }
}

}  // namespace

HttpServer::HttpServer(
    std::string_view address,
    std::uint16_t port,
    HttpServerConfig config,
    HttpHandler handler)
    : HttpServer(
          address,
          port,
          std::move(config),
          HttpDeferringHandler(
              [handler = std::move(handler)](
                  const Http1Request& request,
                  HttpResponseToken) -> HttpHandlerResult {
                  return handler(request);
              })) {}

HttpServer::HttpServer(
    std::string_view address,
    std::uint16_t port,
    HttpServerConfig config,
    HttpDeferringHandler handler)
    : config_(std::move(config)),
      handler_(std::move(handler)),
      tls_context_(config_.tls_identity),
      listener_(address, port),
      event_loop_(make_default_event_loop()) {
    event_loop_->add(
        to_event_loop_handle(listener_.native_handle()), EventInterest::Read);
}

HttpServer::~HttpServer() = default;

std::uint16_t HttpServer::local_port() const noexcept {
    return listener_.local_port();
}

void HttpServer::poll_once(std::chrono::milliseconds timeout) {
    const auto earliest = time_to_earliest_deadline();
    const auto wait = std::min(timeout, earliest);
    const auto ready_events = event_loop_->wait(wait);
    const auto listener_handle =
        to_event_loop_handle(listener_.native_handle());
    for (const auto& ready : ready_events) {
        if (ready.handle == listener_handle) {
            if (ready.readable) {
                accept_connections();
            }
            continue;
        }
        const auto found = connections_.find(ready.handle);
        if (found != connections_.end()) {
            service_connection(found->second, ready);
        }
    }
    sweep_deadlines();
}

void HttpServer::accept_connections() {
    while (true) {
        auto socket = listener_.accept();
        if (!socket.has_value()) {
            return;
        }
        if (connections_.size() >= config_.max_connections) {
            continue;  // 背压:超限即弃(析构关闭)。
        }
        const auto handle = to_event_loop_handle(socket->native_handle());
        const auto now = std::chrono::steady_clock::now();
        event_loop_->add(handle, EventInterest::Read);
        connections_.try_emplace(
            handle,
            Connection{
                .connection = TlsConnection(
                    std::move(*socket),
                    tls_context_.native_handle(),
                    config_.max_head_bytes + config_.max_body_bytes,
                    config_.max_pending_output_bytes),
                .last_activity = now,
                .parse_started_at = now});
    }
}

void HttpServer::service_connection(
    Connection& connection,
    const ReadyEvent& ready) {
    if (ready.error) {
        close_connection(connection);
        return;
    }
    if (!connection.handshake_complete) {
        connection.io_need = connection.connection.accept_handshake();
        if (connection.io_need == TlsIoState::Ready) {
            connection.handshake_complete = true;
            connection.last_activity = std::chrono::steady_clock::now();
        } else if (
            connection.io_need == TlsIoState::Closed ||
            connection.io_need == TlsIoState::Failed) {
            close_connection(connection);
            return;
        }
    }
    if (connection.handshake_complete && ready.readable) {
        auto received = connection.connection.receive_stream();
        connection.io_need = received.state;
        if (received.state == TlsIoState::Failed) {
            close_connection(connection);
            return;
        }
        if (received.status == ReceiveStatus::PeerClosed) {
            // 对端只关了写方向:已收请求(含挂起中的)照常应答,写完再关。
            connection.peer_closed = true;
        }
        bool rejected = false;
        if (!received.bytes.empty()) {
            if (!connection.parse_in_progress &&
                !connection.awaiting.has_value()) {
                // 截止自请求首字节起算,不随后续字节延长——慢速发送受限。
                connection.parse_started_at = std::chrono::steady_clock::now();
                connection.parse_in_progress = true;
            }
            connection.input.append(received.bytes);
            if (connection.input.readable_bytes() >
                connection.parser.max_input_bytes()) {
                if (connection.awaiting.has_value()) {
                    // 回绝响应会插到挂起响应之前,乱序;直接关闭。
                    close_connection(connection);
                    return;
                }
                reject_and_close(connection, Http1ParseStatus::HeadersTooLarge);
                rejected = true;
            }
        }
        if (!rejected) {
            if (!parse_and_dispatch(connection)) {
                return;
            }
        }
    }
    flush_and_settle(connection, ready.writable);
}

bool HttpServer::parse_and_dispatch(Connection& connection) {
    while (!connection.awaiting.has_value() && !connection.close_after_flush) {
        auto result = connection.parser.try_parse(connection.input);
        if (result.status == Http1ParseStatus::NeedMoreData) {
            break;
        }
        if (result.status != Http1ParseStatus::RequestReady) {
            reject_and_close(connection, result.status);
            break;
        }
        if (!dispatch_request(connection, std::move(*result.request))) {
            return false;  // 背压关连接,connection 已析构。
        }
    }
    return true;
}

void HttpServer::flush_and_settle(Connection& connection, bool writable) {
    if (connection.handshake_complete &&
        (writable || connection.connection.has_pending_output())) {
        connection.io_need = connection.connection.flush_output();
        if (connection.io_need == TlsIoState::Closed ||
            connection.io_need == TlsIoState::Failed) {
            close_connection(connection);
            return;
        }
    }
    if ((connection.close_after_flush || connection.peer_closed) &&
        !connection.awaiting.has_value() &&
        !connection.connection.has_pending_output()) {
        close_connection(connection);
        return;
    }
    update_interest(connection);
}

/// 返回 false 表示连接已因输出背压关闭(connection 已析构),
/// 调用方必须立即返回,不得再触碰。
bool HttpServer::dispatch_request(
    Connection& connection,
    Http1Request request) {
    const auto now = std::chrono::steady_clock::now();
    connection.parse_in_progress = false;
    connection.last_activity = now;
    const bool keep_alive = request.wants_keep_alive();
    const HttpResponseToken token = next_token_++;
    HttpHandlerResult result;
    try {
        result = handler_(request, token);
    } catch (...) {
        result = Http1Response{.status = 500, .headers = {}, .body = ""};
    }
    if (std::holds_alternative<HttpDeferred>(result)) {
        connection.awaiting = token;
        connection.awaiting_keep_alive = keep_alive;
        awaiting_.emplace(
            token,
            to_event_loop_handle(connection.connection.native_handle()));
        return true;
    }
    return queue_response(
        connection, std::get<Http1Response>(result), keep_alive);
}

bool HttpServer::queue_response(
    Connection& connection,
    const Http1Response& response,
    bool keep_alive) {
    connection.close_after_flush = connection.close_after_flush || !keep_alive;
    const auto wire = serialize_http1_response(response, keep_alive);
    if (!connection.connection.queue_bytes(
            std::as_bytes(std::span{wire}))) {
        close_connection(connection);
        return false;
    }
    return true;
}

bool HttpServer::complete(HttpResponseToken token, Http1Response response) {
    const auto awaiting = awaiting_.find(token);
    if (awaiting == awaiting_.end()) {
        return false;
    }
    const auto found = connections_.find(awaiting->second);
    awaiting_.erase(awaiting);
    if (found == connections_.end()) {
        return false;
    }
    auto& connection = found->second;
    connection.awaiting.reset();
    const auto now = std::chrono::steady_clock::now();
    connection.last_activity = now;
    if (!queue_response(connection, response, connection.awaiting_keep_alive)) {
        return false;
    }
    // 挂起期间流水线到达的字节:按新请求起算解析截止,继续按序处理。
    if (connection.input.readable_bytes() > 0) {
        connection.parse_started_at = now;
        connection.parse_in_progress = true;
    }
    if (parse_and_dispatch(connection)) {
        flush_and_settle(connection, false);
    }
    return true;
}

void HttpServer::reject_and_close(
    Connection& connection,
    Http1ParseStatus parse_status) {
    // 协议层错误回绝后不再在同一连接上解析。
    connection.close_after_flush = true;
    const auto wire = serialize_http1_response(
        {.status = status_for(parse_status), .headers = {}, .body = ""},
        false);
    static_cast<void>(connection.connection.queue_bytes(
        std::as_bytes(std::span{wire})));
}

void HttpServer::close_connection(Connection& connection) {
    if (connection.awaiting.has_value()) {
        awaiting_.erase(*connection.awaiting);
    }
    const auto handle =
        to_event_loop_handle(connection.connection.native_handle());
    if (connection.registered) {
        event_loop_->remove(handle);
    }
    connections_.erase(handle);
}

void HttpServer::sweep_deadlines() {
    const auto now = std::chrono::steady_clock::now();
    std::vector<EventLoopHandle> expired;
    for (const auto& [handle, connection] : connections_) {
        if (now >= connection_deadline(connection)) {
            expired.push_back(handle);
        }
    }
    for (const auto handle : expired) {
        const auto found = connections_.find(handle);
        if (found != connections_.end()) {
            close_connection(found->second);
        }
    }
}

void HttpServer::update_interest(Connection& connection) {
    const auto handle =
        to_event_loop_handle(connection.connection.native_handle());
    const bool wants_write = connection.io_need == TlsIoState::WantWrite ||
                             connection.connection.has_pending_output();
    if (connection.peer_closed && !wants_write) {
        // 对端已关写方向,只剩挂起响应可等:EOF 在水平触发下恒可读,
        // 摘下句柄免得空转,complete 后再挂回;截止扫描照常生效。
        if (connection.registered) {
            event_loop_->remove(handle);
            connection.registered = false;
        }
        return;
    }
    auto interest = connection.peer_closed ? EventInterest::Write
                                           : EventInterest::Read;
    if (wants_write) {
        interest = interest | EventInterest::Write;
    }
    if (connection.registered) {
        event_loop_->modify(handle, interest);
    } else {
        event_loop_->add(handle, interest);
        connection.registered = true;
    }
}

std::chrono::steady_clock::time_point HttpServer::connection_deadline(
    const Connection& connection) const {
    if (connection.parse_in_progress) {
        return connection.parse_started_at + config_.parse_timeout;
    }
    return connection.last_activity + config_.idle_timeout;
}

std::chrono::milliseconds HttpServer::time_to_earliest_deadline() const {
    if (connections_.empty()) {
        return std::chrono::milliseconds::max();
    }
    const auto now = std::chrono::steady_clock::now();
    auto earliest = std::chrono::milliseconds::max();
    for (const auto& [handle, connection] : connections_) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            connection_deadline(connection) - now);
        earliest = std::min(earliest, remaining);
    }
    return std::max(std::chrono::milliseconds::zero(), earliest);
}

}  // namespace realm::network
