#pragma once

// HTTP/1 词法助手:请求侧(http1_parser)与响应侧(http1_response_parser
// / http1_response)共用的字符集与大小写规则。内部实现头,同目录源文件
// 直接引用,不进公开 include/。

#include <algorithm>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>

namespace realm::network::lexing {

[[nodiscard]] inline std::string_view as_string_view(
    std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

[[nodiscard]] inline char to_lower(char value) {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

[[nodiscard]] inline bool is_tchar(char value) {
    // RFC 9110 tchar:token 允许字符集(方法名与头名共用)。
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') || value == '!' || value == '#' ||
        value == '$' || value == '%' || value == '&' || value == '\'' ||
        value == '*' || value == '+' || value == '-' || value == '.' ||
        value == '^' || value == '_' || value == '`' || value == '|' ||
        value == '~';
}

[[nodiscard]] inline bool is_valid_value_char(char value) {
    // 允许可见 ASCII、HTAB 与 obs-text(%x80-FF);拒其他控制字符。
    return value == '\t' || (value >= 0x20 && value <= 0x7E) ||
        static_cast<unsigned char>(value) >= 0x80U;
}

[[nodiscard]] inline std::string lowercase(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char value : text) {
        lowered.push_back(to_lower(value));
    }
    return lowered;
}

[[nodiscard]] inline bool equals_ignore_case(
    std::string_view left,
    std::string_view right) {
    return left.size() == right.size() && std::equal(
                                              left.begin(),
                                              left.end(),
                                              right.begin(),
                                              [](char lhs, char rhs) {
                                                  return to_lower(lhs) ==
                                                      to_lower(rhs);
                                              });
}

[[nodiscard]] inline std::string_view trim_sp_htab(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

}  // namespace realm::network::lexing
