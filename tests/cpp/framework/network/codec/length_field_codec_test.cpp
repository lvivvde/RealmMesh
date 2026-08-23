#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/network/core/byte_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace realm::network {
namespace {

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    std::ranges::transform(text, std::back_inserter(result), [](char value) {
        return static_cast<std::byte>(value);
    });
    return result;
}

std::string text(std::span<const std::byte> data) {
    std::string result;
    result.reserve(data.size());
    std::ranges::transform(
        data, std::back_inserter(result), [](std::byte value) {
            return static_cast<char>(value);
        });
    return result;
}

TEST(LengthFieldCodecTest, EncodesFourByteBigEndianLength) {
    const LengthFieldCodec codec(1024);
    const auto encoded = codec.encode(bytes("abc"));

    const std::vector<std::byte> expected{
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x03},
        std::byte{'a'},
        std::byte{'b'},
        std::byte{'c'},
    };
    EXPECT_EQ(encoded, expected);
}

TEST(LengthFieldCodecTest, WaitsForAFragmentedFrameWithoutConsumingData) {
    const LengthFieldCodec codec(1024);
    const auto encoded = codec.encode(bytes("hello"));
    ByteBuffer buffer;

    buffer.append(std::span{encoded}.first(2));
    EXPECT_EQ(codec.try_decode(buffer).status, DecodeStatus::NeedMoreData);
    EXPECT_EQ(buffer.readable_bytes(), 2U);

    buffer.append(std::span{encoded}.subspan(2, 3));
    EXPECT_EQ(codec.try_decode(buffer).status, DecodeStatus::NeedMoreData);
    EXPECT_EQ(buffer.readable_bytes(), 5U);

    buffer.append(std::span{encoded}.subspan(5));
    const auto decoded = codec.try_decode(buffer);

    EXPECT_EQ(decoded.status, DecodeStatus::FrameReady);
    EXPECT_EQ(text(decoded.payload), "hello");
    EXPECT_TRUE(buffer.empty());
}

TEST(LengthFieldCodecTest, DecodesCoalescedFramesOneAtATime) {
    const LengthFieldCodec codec(1024);
    const auto first = codec.encode(bytes("one"));
    const auto second = codec.encode(bytes("two"));
    ByteBuffer buffer;

    buffer.append(first);
    buffer.append(second);

    const auto first_result = codec.try_decode(buffer);
    const auto second_result = codec.try_decode(buffer);

    EXPECT_EQ(text(first_result.payload), "one");
    EXPECT_EQ(text(second_result.payload), "two");
    EXPECT_TRUE(buffer.empty());
}

TEST(LengthFieldCodecTest, SupportsAnEmptyPayload) {
    const LengthFieldCodec codec(1024);
    ByteBuffer buffer;
    buffer.append(codec.encode({}));

    const auto decoded = codec.try_decode(buffer);

    EXPECT_EQ(decoded.status, DecodeStatus::FrameReady);
    EXPECT_TRUE(decoded.payload.empty());
    EXPECT_TRUE(buffer.empty());
}

TEST(LengthFieldCodecTest, ReportsOversizedFrameWithoutAllocatingPayload) {
    const LengthFieldCodec codec(4);
    ByteBuffer buffer;
    const std::vector<std::byte> oversized_header{
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x00},
        std::byte{0x05},
    };
    buffer.append(oversized_header);

    const auto decoded = codec.try_decode(buffer);

    EXPECT_EQ(decoded.status, DecodeStatus::FrameTooLarge);
    EXPECT_TRUE(decoded.payload.empty());
    EXPECT_EQ(buffer.readable_bytes(), oversized_header.size());
}

TEST(LengthFieldCodecTest, RefusesToEncodeOversizedPayload) {
    const LengthFieldCodec codec(4);

    EXPECT_THROW(codec.encode(bytes("12345")), std::length_error);
}

}  // namespace
}  // namespace realm::network
