#include "realmmesh/game/common/base64url.hpp"

#include <cstdint>

namespace realm::game::common {
namespace {

constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
                            "abcdefghijklmnopqrstuvwxyz0123456789-_";

int alphabet_index(char value) {
    if (value >= 'A' && value <= 'Z') return value - 'A';
    if (value >= 'a' && value <= 'z') return value - 'a' + 26;
    if (value >= '0' && value <= '9') return value - '0' + 52;
    if (value == '-') return 62;
    if (value == '_') return 63;
    return -1;
}

}  // namespace

std::string base64url_encode(std::span<const std::byte> bytes) {
    std::string encoded;
    encoded.reserve((bytes.size() + 2) / 3 * 4);
    std::size_t index = 0;
    while (index + 3 <= bytes.size()) {
        const auto group =
            (std::to_integer<std::uint32_t>(bytes[index]) << 16U) |
            (std::to_integer<std::uint32_t>(bytes[index + 1]) << 8U) |
            std::to_integer<std::uint32_t>(bytes[index + 2]);
        encoded.push_back(alphabet[(group >> 18U) & 0x3fU]);
        encoded.push_back(alphabet[(group >> 12U) & 0x3fU]);
        encoded.push_back(alphabet[(group >> 6U) & 0x3fU]);
        encoded.push_back(alphabet[group & 0x3fU]);
        index += 3;
    }
    const auto remainder = bytes.size() - index;
    if (remainder == 1) {
        const auto value = std::to_integer<std::uint32_t>(bytes[index]);
        // 尾组低位补零,与解码侧的余位校验互为镜像。
        encoded.push_back(alphabet[(value >> 2U) & 0x3fU]);
        encoded.push_back(alphabet[(value << 4U) & 0x3fU]);
    } else if (remainder == 2) {
        const auto value =
            (std::to_integer<std::uint32_t>(bytes[index]) << 8U) |
            std::to_integer<std::uint32_t>(bytes[index + 1]);
        encoded.push_back(alphabet[(value >> 10U) & 0x3fU]);
        encoded.push_back(alphabet[(value >> 4U) & 0x3fU]);
        encoded.push_back(alphabet[(value << 2U) & 0x3fU]);
    }
    return encoded;
}

std::optional<std::vector<std::byte>> base64url_decode(std::string_view text) {
    if (text.size() % 4 == 1) {
        return std::nullopt;  // 6 bit 组无法落进整字节。
    }
    std::vector<std::byte> decoded;
    decoded.reserve(text.size() / 4 * 3 + 2);
    std::uint32_t buffer = 0;
    unsigned pending_bits = 0;
    for (const char value : text) {
        const int digit = alphabet_index(value);
        if (digit < 0) {
            return std::nullopt;
        }
        buffer = (buffer << 6U) | static_cast<std::uint32_t>(digit);
        pending_bits += 6;
        if (pending_bits >= 8) {
            pending_bits -= 8;
            decoded.push_back(
                static_cast<std::byte>((buffer >> pending_bits) & 0xffU));
        }
    }
    if (pending_bits > 0 && (buffer & ((1U << pending_bits) - 1U)) != 0) {
        return std::nullopt;  // 余位非零:不是任何输入的规范编码。
    }
    return decoded;
}

}  // namespace realm::game::common
