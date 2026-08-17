#pragma once

#include "realmmesh/network/core/byte_buffer.hpp"

#include <cstddef>
#include <span>
#include <vector>

namespace realm::network {

enum class DecodeStatus {
    FrameReady,
    NeedMoreData,
    FrameTooLarge,
};

struct DecodeResult {
    DecodeStatus status;
    std::vector<std::byte> payload;
};

class LengthFieldCodec final {
public:
    static constexpr std::size_t header_size = 4;

    explicit LengthFieldCodec(std::size_t max_payload_size);

    [[nodiscard]] std::vector<std::byte> encode(
        std::span<const std::byte> payload) const;
    [[nodiscard]] DecodeResult try_decode(ByteBuffer& buffer) const;

private:
    std::size_t max_payload_size_;
};

}  // namespace realm::network
