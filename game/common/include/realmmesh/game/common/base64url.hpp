#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace realm::game::common {

/// RFC 4648 §5 base64url 变体,无填充:身份 Token 的头/载荷/签名段与
/// JWKS 的 x 字段共用同一条编码路径。编码只产规范形(余位为 0),
/// 解码只认规范形——'=' 填充、字母表外字符与非零余位一律拒绝。
[[nodiscard]] std::string base64url_encode(std::span<const std::byte> bytes);
[[nodiscard]] std::optional<std::vector<std::byte>> base64url_decode(
    std::string_view text);

}  // namespace realm::game::common
