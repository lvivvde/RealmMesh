#pragma once

#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"
#include "realmmesh/network/tcp/tcp_socket.hpp"

#include <memory>
#include <span>
#include <vector>

typedef struct ssl_st SSL;
typedef struct ssl_ctx_st SSL_CTX;

namespace realm::network {

enum class TlsIoState {
    Ready,
    WantRead,
    WantWrite,
    Closed,
    Failed,
};

enum class ReceiveStatus {
    Open,
    PeerClosed,
    FrameTooLarge,
};

struct ReceiveBatch {
    ReceiveStatus status;
    std::vector<std::vector<std::byte>> frames;
};

struct TlsReceiveBatch {
    TlsIoState state{TlsIoState::Ready};
    ReceiveBatch batch{ReceiveStatus::Open, {}};
};

class TlsConnection final {
public:
    TlsConnection(
        TcpSocket socket,
        SSL_CTX* context,
        std::size_t max_payload_size,
        std::size_t max_pending_output_bytes);

    TlsConnection(const TlsConnection&) = delete;
    TlsConnection& operator=(const TlsConnection&) = delete;
    TlsConnection(TlsConnection&&) noexcept = default;
    TlsConnection& operator=(TlsConnection&&) noexcept = default;

    [[nodiscard]] int native_handle() const noexcept;
    [[nodiscard]] TlsIoState accept_handshake();
    [[nodiscard]] TlsReceiveBatch receive_frames();
    [[nodiscard]] bool queue_frame(std::span<const std::byte> payload);
    [[nodiscard]] TlsIoState flush_output();
    [[nodiscard]] bool has_pending_output() const noexcept;

private:
    struct Deleter {
        void operator()(SSL* ssl) const noexcept;
    };

    [[nodiscard]] bool decode_available(ReceiveBatch& batch);
    [[nodiscard]] static TlsIoState classify_result(SSL* ssl, int result);

    TcpSocket socket_;
    std::unique_ptr<SSL, Deleter> ssl_;
    LengthFieldCodec codec_;
    std::size_t max_pending_output_bytes_;
    ByteBuffer input_;
    ByteBuffer output_;
};

}  // namespace realm::network
