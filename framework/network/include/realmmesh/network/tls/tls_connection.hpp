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

/// 流模式接收结果:解密后的裸字节流,无帧封装(HTTP 边使用)。
struct TlsStreamBatch {
    TlsIoState state{TlsIoState::Ready};
    ReceiveStatus status{ReceiveStatus::Open};
    std::vector<std::byte> bytes;
};

/// 单条 TLS 连接的收发状态机(唯一的 SSL 状态分类处)。收与发各有两条
/// 互斥路径:帧路径(LengthFieldCodec,消息传输默认)与流路径(HTTP 边,
/// 裸字节流);同一连接只走其中一种组合。
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
    [[nodiscard]] TlsStreamBatch receive_stream();
    [[nodiscard]] bool queue_frame(std::span<const std::byte> payload);
    /// 流模式入队:不做帧封装,受同一输出高水位约束。
    [[nodiscard]] bool queue_bytes(std::span<const std::byte> payload);
    [[nodiscard]] TlsIoState flush_output();
    [[nodiscard]] bool has_pending_output() const noexcept;

private:
    struct Deleter {
        void operator()(SSL* ssl) const noexcept;
    };

    [[nodiscard]] bool decode_available(ReceiveBatch& batch);
    [[nodiscard]] bool fits_pending_output(std::size_t payload_size)
        const noexcept;
    [[nodiscard]] static TlsIoState classify_result(SSL* ssl, int result);

    TcpSocket socket_;
    std::unique_ptr<SSL, Deleter> ssl_;
    LengthFieldCodec codec_;
    std::size_t max_pending_output_bytes_;
    ByteBuffer input_;
    ByteBuffer output_;
};

}  // namespace realm::network
