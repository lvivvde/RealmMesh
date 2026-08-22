#include "realmmesh/network/tls/tls_connection.hpp"

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace realm::network {

TlsConnection::TlsConnection(
    TcpSocket socket,
    SSL_CTX* context,
    std::size_t max_payload_size,
    std::size_t max_pending_output_bytes)
    : socket_(std::move(socket)),
      ssl_(SSL_new(context)),
      codec_(max_payload_size),
      max_pending_output_bytes_(max_pending_output_bytes) {
    if (!ssl_) {
        throw std::runtime_error("SSL_new failed");
    }
    if (max_pending_output_bytes_ == 0) {
        throw std::invalid_argument("TLS/TCP output high watermark must be positive");
    }
    if (SSL_set_fd(ssl_.get(), socket_.native_handle()) != 1) {
        throw std::runtime_error("SSL_set_fd failed");
    }
    SSL_set_accept_state(ssl_.get());
}

void TlsConnection::Deleter::operator()(SSL* ssl) const noexcept {
    SSL_free(ssl);
}

int TlsConnection::native_handle() const noexcept {
    return socket_.native_handle();
}

TlsIoState TlsConnection::accept_handshake() {
    const int result = SSL_accept(ssl_.get());
    if (result != 1) {
        return classify_result(ssl_.get(), result);
    }
    const unsigned char* selected_alpn = nullptr;
    unsigned int selected_alpn_size = 0;
    SSL_get0_alpn_selected(ssl_.get(), &selected_alpn, &selected_alpn_size);
    return selected_alpn_size == 0U ? TlsIoState::Failed : TlsIoState::Ready;
}

TlsReceiveBatch TlsConnection::receive_frames() {
    TlsReceiveBatch result;
    std::array<std::byte, 8192> receive_buffer{};
    while (true) {
        std::size_t received = 0;
        const int status = SSL_read_ex(
            ssl_.get(), receive_buffer.data(), receive_buffer.size(), &received);
        if (status == 1) {
            input_.append(std::span<const std::byte>{receive_buffer.data(), received});
            if (!decode_available(result.batch)) {
                result.state = TlsIoState::Failed;
                return result;
            }
            continue;
        }
        result.state = classify_result(ssl_.get(), status);
        if (result.state == TlsIoState::Closed) {
            result.batch.status = ReceiveStatus::PeerClosed;
        }
        return result;
    }
}

bool TlsConnection::queue_frame(std::span<const std::byte> payload) {
    const auto frame = codec_.encode(payload);
    const auto available = max_pending_output_bytes_ -
                           std::min(max_pending_output_bytes_, output_.readable_bytes());
    if (frame.size() > available) {
        return false;
    }
    output_.append(frame);
    return true;
}

TlsIoState TlsConnection::flush_output() {
    while (!output_.empty()) {
        const auto pending = output_.readable_data();
        std::size_t written = 0;
        const int result = SSL_write_ex(
            ssl_.get(), pending.data(), pending.size(), &written);
        if (result == 1) {
            output_.consume(written);
            continue;
        }
        return classify_result(ssl_.get(), result);
    }
    return TlsIoState::Ready;
}

bool TlsConnection::has_pending_output() const noexcept {
    return !output_.empty();
}

bool TlsConnection::decode_available(ReceiveBatch& batch) {
    while (true) {
        auto decoded = codec_.try_decode(input_);
        switch (decoded.status) {
        case DecodeStatus::FrameReady:
            batch.frames.push_back(std::move(decoded.payload));
            break;
        case DecodeStatus::NeedMoreData:
            return true;
        case DecodeStatus::FrameTooLarge:
            batch.status = ReceiveStatus::FrameTooLarge;
            return false;
        }
    }
}

TlsIoState TlsConnection::classify_result(SSL* ssl, int result) {
    switch (SSL_get_error(ssl, result)) {
    case SSL_ERROR_WANT_READ:
        return TlsIoState::WantRead;
    case SSL_ERROR_WANT_WRITE:
        return TlsIoState::WantWrite;
    case SSL_ERROR_ZERO_RETURN:
        return TlsIoState::Closed;
    default:
        return TlsIoState::Failed;
    }
}

}  // namespace realm::network
