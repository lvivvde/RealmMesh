#pragma once

namespace realm::game::common::hex {

/// 单个 hex 字符 → 0–15,大小写均可;其余返回 -1。
/// 会话票据密钥与身份种子共用的最小解码原语(src 内部,不进安装面)。
inline int nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

}  // namespace realm::game::common::hex
