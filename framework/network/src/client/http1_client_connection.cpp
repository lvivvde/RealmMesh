#include "realmmesh/network/client/http1_client_connection.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace realm::network::client {
namespace {

constexpr std::size_t kReadChunk = 4096;

[[nodiscard]] std::string encode_request(std::string_view method,
                                        std::string_view target,
                                        std::string_view host_header,
                                        const std::optional<std::string>& bearer,
                                        std::string_view body) {
    std::string request;
    request.reserve(256 + host_header.size() + body.size() +
                    (bearer.has_value() ? bearer->size() : 0));
    request.append(method);
    request.push_back(' ');
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append(host_header);
    request.append("\r\n");
    if (bearer.has_value()) {
        request.append("Authorization: Bearer ");
        request.append(*bearer);
        request.append("\r\n");
    }
    if (!body.empty()) {
        request.append("Content-Type: application/json\r\n");
        request.append("Content-Length: ");
        request.append(std::to_string(body.size()));
        request.append("\r\n");
    }
    request.append("Connection: keep-alive\r\n\r\n");
    request.append(body);
    return request;
}

}  // namespace

ClientDialResult Http1ClientConnection::dial(
    std::string_view host,
    std::uint16_t port,
    const TlsClientOptions& options,
    StreamDeadline deadline,
    std::stop_token stop) {
    TlsClientOptions http_options = options;
    if (http_options.alpn.empty()) {
        http_options.alpn = "http/1.1";
    }
    auto result = TlsClientStream::dial(host, port, http_options, deadline, stop);
    return ClientDialResult{std::move(result.stream), result.failure,
                            result.last_errno, result.verify_result};
}

Http1ClientConnection::Http1ClientConnection(
    std::shared_ptr<ISecureByteStream> stream)
    : stream_(std::move(stream)) {}

Http1ClientConnection::~Http1ClientConnection() = default;

std::optional<network::Http1Response> Http1ClientConnection::request(
    std::string_view method,
    std::string_view target,
    std::string_view host_header,
    const std::optional<std::string>& bearer,
    std::string_view body,
    StreamDeadline deadline) {
    const std::string request =
        encode_request(method, target, host_header, bearer, body);
    const auto bytes = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(request.data()), request.size());
    if (!stream_->write_all(bytes, deadline)) {
        return std::nullopt;
    }

    // 逐块收包直到解析出完整响应;解析器错误与对端断开一并视为网络层
    // 失败(连接作废,由调用方重建)。
    for (;;) {
        auto parsed = parser_.try_parse(pending_);
        if (parsed.status ==
            network::Http1ResponseParseStatus::ResponseReady) {
            return std::move(parsed.response);
        }
        if (parsed.status !=
            network::Http1ResponseParseStatus::NeedMoreData) {
            return std::nullopt;
        }
        std::array<std::byte, kReadChunk> chunk{};
        const auto received = stream_->read_some(chunk, deadline);
        if (!received.has_value() || *received == 0) {
            return std::nullopt;
        }
        pending_.append(std::span<const std::byte>(chunk.data(), *received));
    }
}

}  // namespace realm::network::client
