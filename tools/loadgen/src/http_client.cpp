#include "realmmesh/loadgen/http_client.hpp"

#include "tls_wire.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"
#include "realmmesh/network/http/http1_response_parser.hpp"

#include <openssl/ssl.h>

#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace realm::loadgen {

struct TlsHttpConnection::Impl final {
    tls_wire::Stream stream;
    network::Http1ResponseParser parser;
};

std::unique_ptr<TlsHttpConnection> TlsHttpConnection::dial(
    const ServiceAddress& address,
    std::chrono::steady_clock::time_point deadline) {
    auto impl = std::make_unique<Impl>();
    if (!tls_wire::dial_stream(address, "http/1.1", deadline, impl->stream)) {
        return nullptr;
    }
    return std::unique_ptr<TlsHttpConnection>(
        new TlsHttpConnection(std::move(impl)));
}

TlsHttpConnection::TlsHttpConnection(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TlsHttpConnection::~TlsHttpConnection() = default;

std::optional<network::Http1Response> TlsHttpConnection::request(
    std::string_view method,
    std::string_view target,
    const std::optional<std::string>& bearer,
    std::string_view body,
    std::chrono::steady_clock::time_point deadline) {
    std::string request;
    request.reserve(256 + body.size());
    request.append(method);
    request.push_back(' ');
    request.append(target);
    request.append(" HTTP/1.1\r\nHost: ");
    request.append("loadgen.realmmesh.example\r\n");
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

    if (!tls_wire::write_all(
            impl_->stream,
            reinterpret_cast<const unsigned char*>(request.data()),
            request.size(),
            deadline)) {
        return std::nullopt;
    }

    // 逐块收包直到解析出完整响应;解析器错误与对端断开一并视为
    // 网络层失败(连接作废,由调用方重建)。
    network::ByteBuffer buffer;
    for (;;) {
        auto parsed = impl_->parser.try_parse(buffer);
        if (parsed.status == network::Http1ResponseParseStatus::ResponseReady) {
            return std::move(parsed.response);
        }
        if (parsed.status !=
            network::Http1ResponseParseStatus::NeedMoreData) {
            return std::nullopt;
        }
        std::array<std::byte, 4096> chunk{};
        std::size_t received = 0;
        const int result = SSL_read_ex(
            impl_->stream.ssl.get(), chunk.data(), chunk.size(), &received);
        if (result == 1) {
            buffer.append(
                std::span<const std::byte>(chunk.data(), received));
            continue;
        }
        const int error =
            SSL_get_error(impl_->stream.ssl.get(), result);
        if ((error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            tls_wire::wait_ssl_ready(
                impl_->stream.descriptor, error, deadline)) {
            continue;
        }
        return std::nullopt;
    }
}

}  // namespace realm::loadgen
