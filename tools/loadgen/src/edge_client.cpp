#include "realmmesh/loadgen/edge_client.hpp"

#include "tls_wire.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"

#include <openssl/ssl.h>

#include <sys/socket.h>

#include <array>
#include <cstddef>
#include <span>
#include <utility>

namespace realm::loadgen {

/// 单帧负载上限:attach 与 1303 授权(票据 + 端点)在 1KB 内,
/// 上限放宽到 16KB 只为容忍端点列表扩展。
constexpr std::size_t kMaxFramePayload = 16 * 1024;

struct TlsEdgeConnection::Impl final {
    tls_wire::Stream stream;
    network::LengthFieldCodec codec{kMaxFramePayload};
    std::vector<std::byte> pending;
};

std::unique_ptr<TlsEdgeConnection> TlsEdgeConnection::dial(
    const ServiceAddress& address,
    std::chrono::steady_clock::time_point deadline) {
    auto impl = std::make_unique<Impl>();
    if (!tls_wire::dial_stream(
            address, "realmmesh-edge/1", deadline, impl->stream)) {
        return nullptr;
    }
    return std::unique_ptr<TlsEdgeConnection>(
        new TlsEdgeConnection(std::move(impl)));
}

TlsEdgeConnection::TlsEdgeConnection(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

TlsEdgeConnection::~TlsEdgeConnection() = default;

bool TlsEdgeConnection::send_frame(
    std::span<const std::byte> payload,
    std::chrono::steady_clock::time_point deadline) {
    const auto framed = impl_->codec.encode(payload);
    return tls_wire::write_all(
        impl_->stream,
        reinterpret_cast<const unsigned char*>(framed.data()),
        framed.size(),
        deadline);
}

std::optional<std::vector<std::byte>> TlsEdgeConnection::receive_frame(
    std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        network::ByteBuffer buffer;
        buffer.append(impl_->pending);
        const auto decoded = impl_->codec.try_decode(buffer);
        if (decoded.status == network::DecodeStatus::FrameReady) {
            const auto consumed = impl_->pending.size() - buffer.readable_bytes();
            impl_->pending.erase(
                impl_->pending.begin(),
                impl_->pending.begin() +
                    static_cast<std::ptrdiff_t>(consumed));
            return decoded.payload;
        }
        if (decoded.status == network::DecodeStatus::FrameTooLarge) {
            return std::nullopt;
        }
        std::array<std::byte, 4096> chunk{};
        std::size_t received = 0;
        const int result = SSL_read_ex(
            impl_->stream.ssl.get(), chunk.data(), chunk.size(), &received);
        if (result == 1) {
            impl_->pending.insert(
                impl_->pending.end(),
                chunk.begin(),
                chunk.begin() + static_cast<std::ptrdiff_t>(received));
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
