#include "realmmesh/network/client/edge_client_connection.hpp"

#include "realmmesh/network/core/byte_buffer.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace realm::network::client {
namespace {

constexpr std::size_t kReadChunk = 4096;
constexpr std::string_view kEdgeAlpn = "realmmesh-edge/1";

}  // namespace

ClientDialResult EdgeClientConnection::dial(std::string_view host,
                                            std::uint16_t port,
                                            const TlsClientOptions& options,
                                            StreamDeadline deadline,
                                            std::stop_token stop) {
    TlsClientOptions edge_options = options;
    if (edge_options.alpn.empty()) {
        edge_options.alpn = std::string{kEdgeAlpn};
    }
    auto result =
        TlsClientStream::dial(host, port, edge_options, deadline, stop);
    return ClientDialResult{std::move(result.stream), result.failure,
                            result.last_errno, result.verify_result};
}

EdgeClientConnection::EdgeClientConnection(
    std::shared_ptr<ISecureByteStream> stream)
    : stream_(std::move(stream)) {}

EdgeClientConnection::~EdgeClientConnection() = default;

bool EdgeClientConnection::send_frame(std::span<const std::byte> payload,
                                      StreamDeadline deadline) {
    const auto framed = codec_.encode(payload);
    return stream_->write_all(framed, deadline);
}

std::optional<std::vector<std::byte>> EdgeClientConnection::receive_frame(
    StreamDeadline deadline) {
    for (;;) {
        network::ByteBuffer buffer;
        buffer.append(pending_);
        const auto decoded = codec_.try_decode(buffer);
        if (decoded.status == network::DecodeStatus::FrameReady) {
            const auto consumed =
                pending_.size() - buffer.readable_bytes();
            pending_.erase(pending_.begin(),
                           pending_.begin() +
                               static_cast<std::ptrdiff_t>(consumed));
            return decoded.payload;
        }
        if (decoded.status == network::DecodeStatus::FrameTooLarge) {
            return std::nullopt;
        }
        std::array<std::byte, kReadChunk> chunk{};
        const auto received = stream_->read_some(chunk, deadline);
        if (!received.has_value() || *received == 0) {
            return std::nullopt;
        }
        pending_.insert(pending_.end(), chunk.begin(),
                        chunk.begin() +
                            static_cast<std::ptrdiff_t>(*received));
    }
}

}  // namespace realm::network::client
