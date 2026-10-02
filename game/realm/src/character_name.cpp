#include "realmmesh/game/realm/character_name.hpp"

#include <cstdint>
#include <optional>

namespace realm::game::realm {
namespace {

/// 解码一个 UTF-8 码点并前移 position;非最短编码、代理区、超出
/// U+10FFFF 或截断一律视为非法。
[[nodiscard]] std::optional<char32_t> next_code_point(
    std::string_view text, std::size_t& position) noexcept {
    const auto byte = [&](std::size_t index) {
        return static_cast<std::uint8_t>(text[index]);
    };
    const auto lead = byte(position);
    std::size_t length = 0;
    char32_t value = 0;
    char32_t minimum = 0;
    if (lead < 0x80) {
        ++position;
        return lead;
    }
    if ((lead & 0xE0) == 0xC0) {
        length = 2;
        value = lead & 0x1F;
        minimum = 0x80;
    } else if ((lead & 0xF0) == 0xE0) {
        length = 3;
        value = lead & 0x0F;
        minimum = 0x800;
    } else if ((lead & 0xF8) == 0xF0) {
        length = 4;
        value = lead & 0x07;
        minimum = 0x10000;
    } else {
        return std::nullopt;
    }
    if (text.size() - position < length) return std::nullopt;
    for (std::size_t index = 1; index < length; ++index) {
        const auto continuation = byte(position + index);
        if ((continuation & 0xC0) != 0x80) return std::nullopt;
        value = (value << 6) | (continuation & 0x3F);
    }
    if (value < minimum || value > 0x10FFFF ||
        (value >= 0xD800 && value <= 0xDFFF)) {
        return std::nullopt;
    }
    position += length;
    return value;
}

[[nodiscard]] constexpr bool is_control(char32_t value) noexcept {
    return value < 0x20 || (value >= 0x7F && value <= 0x9F);
}

/// Unicode White_Space 属性(PropList.txt)。
[[nodiscard]] constexpr bool is_white_space(char32_t value) noexcept {
    return (value >= 0x09 && value <= 0x0D) || value == 0x20 ||
           value == 0x85 || value == 0xA0 || value == 0x1680 ||
           (value >= 0x2000 && value <= 0x200A) || value == 0x2028 ||
           value == 0x2029 || value == 0x202F || value == 0x205F ||
           value == 0x3000;
}

}  // namespace

bool is_valid_character_name(std::string_view name) noexcept {
    std::size_t position = 0;
    std::size_t count = 0;
    char32_t first = 0;
    char32_t last = 0;
    while (position < name.size()) {
        const auto value = next_code_point(name, position);
        if (!value.has_value() || is_control(*value)) return false;
        if (++count > max_character_name_code_points) return false;
        if (count == 1) first = *value;
        last = *value;
    }
    return count > 0 && !is_white_space(first) && !is_white_space(last);
}

}  // namespace realm::game::realm
