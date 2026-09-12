#include "realmmesh/game/common/json.hpp"

#include <limits>

namespace realm::game::common {
namespace {

constexpr char hex_digits[] = "0123456789abcdef";

/// INT64_MIN 的绝对值,比 INT64_MAX 的绝对值大一。
constexpr std::uint64_t int64_min_magnitude =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;

bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

bool is_hex(char c) {
    return is_digit(c) || (c >= 'a' && c <= 'f');
}

unsigned hex_value(char c) {
    return is_digit(c) ? static_cast<unsigned>(c - '0')
                       : static_cast<unsigned>(c - 'a' + 10);
}

/// 转义集单点定义:编码产出与解码接受共用本表,镜像关系由结构保证。
struct EscapePair {
    char control;
    const char* text;
};

constexpr EscapePair escapes[] = {
    {'"', "\\\""}, {'\\', "\\\\"}, {'\b', "\\b"}, {'\f', "\\f"},
    {'\n', "\\n"}, {'\r', "\\r"}, {'\t', "\\t"},
};

const char* find_escape_text(char control) {
    for (const auto& pair : escapes) {
        if (pair.control == control) {
            return pair.text;
        }
    }
    return nullptr;
}

void encode_string(std::string_view value, std::string& out) {
    out.push_back('"');
    for (const char c : value) {
        if (const char* escaped = find_escape_text(c)) {
            out += escaped;
            continue;
        }
        const auto byte = static_cast<unsigned char>(c);
        if (byte < 0x20) {
            out += "\\u00";
            out.push_back(hex_digits[byte >> 4]);
            out.push_back(hex_digits[byte & 0x0f]);
        } else {
            out.push_back(c);
        }
    }
    out.push_back('"');
}

void encode_value(const JsonValue& value, std::string& out) {
    if (const auto* text = std::get_if<std::string>(&value)) {
        encode_string(*text, out);
    } else if (const auto* number = std::get_if<std::int64_t>(&value)) {
        out += std::to_string(*number);
    } else {
        out += std::get<bool>(value) ? "true" : "false";
    }
}

/// 索引式游标;所有读取先查越界,失败即整体拒绝。
class Parser final {
public:
    explicit Parser(std::string_view input) : input_(input) {}

    void skip_whitespace() {
        while (position_ < input_.size() &&
               (input_[position_] == ' ' || input_[position_] == '\t' ||
                input_[position_] == '\n' || input_[position_] == '\r')) {
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < input_.size() && input_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    [[nodiscard]] bool at_end() const {
        return position_ == input_.size();
    }

    std::optional<std::string> parse_string() {
        if (!consume('"')) {
            return std::nullopt;
        }
        std::string out;
        while (position_ < input_.size()) {
            const char c = input_[position_++];
            if (c == '"') {
                return out;
            }
            if (c == '\\') {
                if (!parse_escape(out)) {
                    return std::nullopt;
                }
                continue;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return std::nullopt;
            }
            out.push_back(c);
        }
        return std::nullopt;
    }

    std::optional<JsonValue> parse_value() {
        if (position_ >= input_.size()) {
            return std::nullopt;
        }
        switch (input_[position_]) {
            case '"':
                return parse_string();
            case 't':
                return parse_bool_word("true", true);
            case 'f':
                return parse_bool_word("false", false);
            case '-':
            case '0':
            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9':
                return parse_number();
            default:
                return std::nullopt;
        }
    }

private:
    /// 本语法面拒绝浮点:这两个字符出现即说明是小数或指数。
    [[nodiscard]] bool is_rejected_number_char(char c) const {
        return c == '.' || c == 'e' || c == 'E';
    }

    bool consume_word(std::string_view word) {
        if (input_.substr(position_).starts_with(word)) {
            position_ += word.size();
            return true;
        }
        return false;
    }

    std::optional<JsonValue> parse_bool_word(std::string_view word, bool value) {
        return consume_word(word)
                   ? std::optional<JsonValue>(JsonValue{value})
                   : std::nullopt;
    }

    bool parse_escape(std::string& out) {
        if (position_ >= input_.size()) {
            return false;
        }
        const char c = input_[position_++];
        for (const auto& pair : escapes) {
            if (pair.text[1] == c) {
                out.push_back(pair.control);
                return true;
            }
        }
        if (c == 'u') {
            return parse_unicode_escape(out);
        }
        return false;
    }

    bool parse_unicode_escape(std::string& out) {
        if (input_.size() - position_ < 4) {
            return false;
        }
        unsigned code = 0;
        for (int i = 0; i < 4; ++i) {
            const char c = input_[position_ + static_cast<std::size_t>(i)];
            if (!is_hex(c)) {
                return false;
            }
            code = (code << 4) | hex_value(c);
        }
        position_ += 4;
        if (code >= 0xD800 && code <= 0xDFFF) {
            return false;
        }
        if (code < 0x80) {
            out.push_back(static_cast<char>(code));
        } else if (code < 0x800) {
            out.push_back(static_cast<char>(0xC0 | (code >> 6)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xE0 | (code >> 12)));
            out.push_back(static_cast<char>(0x80 | ((code >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (code & 0x3F)));
        }
        return true;
    }

    std::optional<JsonValue> parse_number() {
        const std::size_t start = position_;
        const bool negative = consume('-');
        if (position_ >= input_.size() || !is_digit(input_[position_])) {
            return std::nullopt;
        }
        if (input_[position_] == '0') {
            ++position_;
            if (position_ < input_.size() && is_digit(input_[position_])) {
                return std::nullopt;
            }
        } else {
            while (position_ < input_.size() && is_digit(input_[position_])) {
                ++position_;
            }
        }
        if (position_ < input_.size() &&
            is_rejected_number_char(input_[position_])) {
            return std::nullopt;
        }
        const std::size_t end = position_;
        std::uint64_t magnitude = 0;
        const std::uint64_t limit =
            negative ? int64_min_magnitude
                     : static_cast<std::uint64_t>(
                           std::numeric_limits<std::int64_t>::max());
        for (std::size_t i = negative ? start + 1U : start; i < end; ++i) {
            const auto digit =
                static_cast<std::uint64_t>(input_[i] - '0');
            if (magnitude > (limit - digit) / 10U) {
                return std::nullopt;
            }
            magnitude = magnitude * 10U + digit;
        }
        if (negative) {
            if (magnitude == int64_min_magnitude) {
                return JsonValue{std::numeric_limits<std::int64_t>::min()};
            }
            return JsonValue{-static_cast<std::int64_t>(magnitude)};
        }
        return JsonValue{static_cast<std::int64_t>(magnitude)};
    }

    std::string_view input_;
    std::size_t position_{0};
};

}  // namespace

std::string JsonCodec::encode(const JsonObject& object) {
    std::string out;
    out.push_back('{');
    bool first = true;
    for (const auto& [key, value] : object) {
        if (!first) {
            out.push_back(',');
        }
        first = false;
        encode_string(key, out);
        out.push_back(':');
        encode_value(value, out);
    }
    out.push_back('}');
    return out;
}

std::optional<JsonObject> JsonCodec::decode(
    std::string_view input,
    std::size_t max_input_size) {
    if (input.size() > max_input_size) {
        return std::nullopt;
    }
    Parser parser(input);
    parser.skip_whitespace();
    if (!parser.consume('{')) {
        return std::nullopt;
    }
    JsonObject object;
    parser.skip_whitespace();
    if (parser.consume('}')) {
        parser.skip_whitespace();
        return parser.at_end()
                   ? std::optional<JsonObject>(std::move(object))
                   : std::nullopt;
    }
    while (true) {
        parser.skip_whitespace();
        auto key = parser.parse_string();
        if (!key.has_value()) {
            return std::nullopt;
        }
        parser.skip_whitespace();
        if (!parser.consume(':')) {
            return std::nullopt;
        }
        parser.skip_whitespace();
        auto value = parser.parse_value();
        if (!value.has_value()) {
            return std::nullopt;
        }
        if (!object.emplace(std::move(*key), std::move(*value)).second) {
            return std::nullopt;
        }
        parser.skip_whitespace();
        if (parser.consume(',')) {
            continue;
        }
        if (parser.consume('}')) {
            break;
        }
        return std::nullopt;
    }
    parser.skip_whitespace();
    if (!parser.at_end()) {
        return std::nullopt;
    }
    return object;
}

}  // namespace realm::game::common
