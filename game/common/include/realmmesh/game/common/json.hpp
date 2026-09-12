#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace realm::game::common {

/// JSON 值,值域受控:字符串 / 64 位整数 / 布尔。
using JsonValue = std::variant<std::string, std::int64_t, bool>;

/// JSON 对象:按键有序的扁平映射,编码输出因此是确定的。
using JsonObject = std::map<std::string, JsonValue>;

/// 极小 JSON 编解码,为身份 Token 与号牌(JWT)的 claims 集及
/// 受控头而设。
/// 只覆盖扁平对象与三值域;嵌套容器、浮点、null、重复键、超限输入
/// 一律拒绝。解析无递归,拒绝是确定性的;解码语义 = 本编码器
/// 产出的语法面(转义集不含 `\/`、代理区 `\uDxxx` 拒收)。
class JsonCodec final {
public:
/// 解码输入上限;超限先于任何语法处理直接拒绝。
static constexpr std::size_t default_max_input_size = 4 * 1024;

JsonCodec() = delete;

/// 编码为紧凑 JSON(无空白),键按字典序输出。字符串转义引号、
/// 反斜杠与控制字符(缩写转义或 \u00XX);UTF-8 其余字节原样通过。
[[nodiscard]] static std::string encode(const JsonObject& object);

/// 解析扁平对象;越限、语法或值域不满足时返回 std::nullopt。
[[nodiscard]] static std::optional<JsonObject> decode(
    std::string_view input,
    std::size_t max_input_size = default_max_input_size);
};

/// 取扁平对象的字符串/整型成员(缺失或类型不符返回 nullptr);供
/// JWS 头与 claims 的逐键校验共用。
[[nodiscard]] const std::string* json_string_member(
    const JsonObject& object, std::string_view key);
[[nodiscard]] const std::int64_t* json_int_member(
    const JsonObject& object, std::string_view key);

}  // namespace realm::game::common
