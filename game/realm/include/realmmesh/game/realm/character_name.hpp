#pragma once

#include <cstddef>
#include <string_view>

namespace realm::game::realm {

inline constexpr std::size_t max_character_name_code_points = 16;

/// 角色名规则(#93):合法 UTF-8,1-16 个 Unicode 码点,不含控制字符
/// (C0、DEL、C1),首尾不是 Unicode 空白。中间空白允许。唯一性按字节
/// 精确比较,由存储的唯一索引保证,不在此处。
[[nodiscard]] bool is_valid_character_name(std::string_view name) noexcept;

}  // namespace realm::game::realm
