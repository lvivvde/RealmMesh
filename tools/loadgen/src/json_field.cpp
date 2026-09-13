#include "realmmesh/loadgen/json_field.hpp"

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

namespace realm::loadgen {
namespace {

/// 定位 `"name"` 后跳过冒号与空白,返回值起点;找不到返回 nullopt。
[[nodiscard]] std::optional<std::size_t> value_start(
    std::string_view body,
    std::string_view name) {
    const auto key = "\"" + std::string{name} + "\"";
    const auto found = body.find(key);
    if (found == std::string_view::npos) {
        return std::nullopt;
    }
    auto rest = body.substr(found + key.size());
    while (!rest.empty() && (rest.front() == ':' || rest.front() == ' ' ||
                             rest.front() == '\t')) {
        rest.remove_prefix(1);
    }
    if (rest.empty()) {
        return std::nullopt;
    }
    return body.size() - rest.size();
}

}  // namespace

std::optional<std::string> extract_json_string_field(
    std::string_view body,
    std::string_view name) {
    const auto start = value_start(body, name);
    if (!start.has_value() || body[*start] != '"') {
        return std::nullopt;
    }
    const auto value = body.substr(*start + 1);
    const auto closing = value.find('"');
    if (closing == std::string_view::npos) {
        return std::nullopt;
    }
    return std::string{value.substr(0, closing)};
}

std::optional<std::int64_t> extract_json_int_field(
    std::string_view body,
    std::string_view name) {
    const auto start = value_start(body, name);
    if (!start.has_value()) {
        return std::nullopt;
    }
    const auto value = body.substr(*start);
    std::int64_t parsed = 0;
    const auto result = std::from_chars(
        value.data(), value.data() + value.size(), parsed);
    if (result.ec != std::errc{} || result.ptr == value.data()) {
        return std::nullopt;
    }
    return parsed;
}

}  // namespace realm::loadgen
