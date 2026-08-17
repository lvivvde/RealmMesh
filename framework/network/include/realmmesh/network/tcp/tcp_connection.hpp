#pragma once

#include "realmmesh/network/core/byte_buffer.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/tcp/tcp_socket.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace realm::network {

enum class ReceiveStatus {
    Open,
    PeerClosed,
    FrameTooLarge,
};

struct ReceiveBatch {
    ReceiveStatus status;
    std::vector<std::vector<std::byte>> frames;
};

class TcpConnection final {
public:
    TcpConnection(
        TcpSocket socket,
        std::size_t max_payload_size,
        std::size_t max_pending_output_bytes);

    TcpConnection(const TcpConnection&) = delete;
    TcpConnection& operator=(const TcpConnection&) = delete;
    TcpConnection(TcpConnection&&) noexcept = default;
    TcpConnection& operator=(TcpConnection&&) noexcept = default;

    [[nodiscard]] int native_handle() const noexcept;
    [[nodiscard]] ReceiveBatch receive_frames();

    [[nodiscard]] bool queue_frame(std::span<const std::byte> payload);
    void flush_output();
    [[nodiscard]] bool has_pending_output() const noexcept;

private:
    [[nodiscard]] bool decode_available(ReceiveBatch& batch);

    TcpSocket socket_;
    LengthFieldCodec codec_;
    std::size_t max_pending_output_bytes_;
    ByteBuffer input_;
    ByteBuffer output_;
};

}  // namespace realm::network
