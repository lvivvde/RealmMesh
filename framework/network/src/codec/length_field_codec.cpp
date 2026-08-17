#include "realmmesh/network/codec/length_field_codec.hpp"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace realm::network {
namespace {

std::uint32_t decode_length(std::span<const std::byte> header) noexcept {
    std::uint32_t length = 0;
    for (const auto value : header) {
        length = static_cast<std::uint32_t>(
            (length << 8U) | std::to_integer<std::uint8_t>(value));
    }
    return length;
}

}  // namespace

LengthFieldCodec::LengthFieldCodec(std::size_t max_payload_size)
    : max_payload_size_(max_payload_size) {}

std::vector<std::byte> LengthFieldCodec::encode(
    std::span<const std::byte> payload) const {
    if (payload.size() > max_payload_size_ ||
        payload.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("payload exceeds configured frame limit");
    }

    const auto length = static_cast<std::uint32_t>(payload.size());
    std::vector<std::byte> encoded;
    encoded.reserve(header_size + payload.size());
    encoded.push_back(static_cast<std::byte>((length >> 24U) & 0xFFU));
    encoded.push_back(static_cast<std::byte>((length >> 16U) & 0xFFU));
    encoded.push_back(static_cast<std::byte>((length >> 8U) & 0xFFU));
    encoded.push_back(static_cast<std::byte>(length & 0xFFU));
    encoded.insert(encoded.end(), payload.begin(), payload.end());
    return encoded;
}

DecodeResult LengthFieldCodec::try_decode(ByteBuffer& buffer) const {
    const auto data = buffer.readable_data();
    if (data.size() < header_size) {
        return {DecodeStatus::NeedMoreData, {}};
    }

    const auto payload_size = static_cast<std::size_t>(
        decode_length(data.first(header_size)));
    if (payload_size > max_payload_size_) {
        return {DecodeStatus::FrameTooLarge, {}};
    }

    const auto frame_size = header_size + payload_size;
    if (data.size() < frame_size) {
        return {DecodeStatus::NeedMoreData, {}};
    }

    const auto payload_view = data.subspan(header_size, payload_size);
    std::vector<std::byte> payload(payload_view.begin(), payload_view.end());
    buffer.consume(frame_size);
    return {DecodeStatus::FrameReady, std::move(payload)};
}

}  // namespace realm::network
