#include "realmmesh/network/tcp/tcp_connection.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <stdexcept>
#include <sys/socket.h>
#include <system_error>
#include <utility>

namespace realm::network {

TcpConnection::TcpConnection(
    TcpSocket socket,
    std::size_t max_payload_size,
    std::size_t max_pending_output_bytes)
    : socket_(std::move(socket)),
      codec_(max_payload_size),
      max_pending_output_bytes_(max_pending_output_bytes) {
    if (max_pending_output_bytes_ == 0) {
        throw std::invalid_argument("TCP output high watermark must be positive");
    }
}

int TcpConnection::native_handle() const noexcept {
    return socket_.native_handle();
}

ReceiveBatch TcpConnection::receive_frames() {
    ReceiveBatch batch{ReceiveStatus::Open, {}};
    std::byte receive_buffer[8192];

    while (true) {
        const auto received = ::recv(
            socket_.native_handle(),
            receive_buffer,
            sizeof(receive_buffer),
            0);
        if (received > 0) {
            input_.append(std::span<const std::byte>{
                receive_buffer,
                static_cast<std::size_t>(received),
            });
            if (!decode_available(batch)) {
                return batch;
            }
            continue;
        }

        if (received == 0) {
            batch.status = ReceiveStatus::PeerClosed;
            return batch;
        }

        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return batch;
        }
        throw std::system_error(errno, std::generic_category(), "recv");
    }
}

bool TcpConnection::queue_frame(std::span<const std::byte> payload) {
    const auto frame = codec_.encode(payload);
    const auto available = max_pending_output_bytes_ -
                           std::min(max_pending_output_bytes_, output_.readable_bytes());
    if (frame.size() > available) {
        return false;
    }
    output_.append(frame);
    return true;
}

void TcpConnection::flush_output() {
    while (!output_.empty()) {
        const auto pending = output_.readable_data();
        const auto sent = ::send(
            socket_.native_handle(),
            pending.data(),
            pending.size(),
            MSG_NOSIGNAL);
        if (sent > 0) {
            output_.consume(static_cast<std::size_t>(sent));
            continue;
        }

        if (sent == 0) {
            return;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return;
        }
        throw std::system_error(errno, std::generic_category(), "send");
    }

}

bool TcpConnection::has_pending_output() const noexcept {
    return !output_.empty();
}

bool TcpConnection::decode_available(ReceiveBatch& batch) {
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

}  // namespace realm::network
