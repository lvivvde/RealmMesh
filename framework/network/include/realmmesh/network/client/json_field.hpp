#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace realm::network::client {

/// 服务响应体的字段提取(轻量扫描):JsonCodec 只编扁平对象,而放行
/// 响应的 admit_grant 是嵌套拼接体;token 值为 base64url/数字等无转义
/// 字符,故按 `"name":"value"` 直接扫描。缺失返回 nullopt。
[[nodiscard]] std::optional<std::string> extract_json_string_field(
    std::string_view body,
    std::string_view name);

/// 整数字段(同上:值无引号,读到首个非数字字符)。
[[nodiscard]] std::optional<std::int64_t> extract_json_int_field(
    std::string_view body,
    std::string_view name);

}  // namespace realm::network::client
