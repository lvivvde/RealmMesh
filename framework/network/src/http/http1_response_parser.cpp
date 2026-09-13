#include "realmmesh/network/http/http1_response_parser.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string_view>

namespace realm::network {
namespace {

constexpr std::array<std::byte, 4> head_terminator{
    std::byte{'\r'}, std::byte{'\n'}, std::byte{'\r'}, std::byte{'\n'}};

std::string_view as_string_view(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

char to_lower(char value) {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

bool is_tchar(char value) {
    // RFC 9110 tchar:token 允许字符集(头名共用)。
    return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'z') ||
        (value >= 'A' && value <= 'Z') || value == '!' || value == '#' ||
        value == '$' || value == '%' || value == '&' || value == '\'' ||
        value == '*' || value == '+' || value == '-' || value == '.' ||
        value == '^' || value == '_' || value == '`' || value == '|' ||
        value == '~';
}

bool is_valid_value_char(char value) {
    // 允许可见 ASCII、HTAB 与 obs-text(%x80-FF);拒其他控制字符。
    return value == '\t' || (value >= 0x20 && value <= 0x7E) ||
        static_cast<unsigned char>(value) >= 0x80U;
}

std::string lowercase(std::string_view text) {
    std::string lowered;
    lowered.reserve(text.size());
    for (const char value : text) {
        lowered.push_back(to_lower(value));
    }
    return lowered;
}

std::string_view trim_sp_htab(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

struct HeadFields {
    Http1Response response;
    std::uint64_t content_length{0};
};

/// 解析状态行:HTTP/1.1 <3 位数字> [reason];reason 可省略。
std::optional<int> parse_status_line(
    std::string_view line,
    Http1ResponseParseStatus& error) {
    const auto first_space = line.find(' ');
    if (first_space == std::string_view::npos ||
        line.substr(0, first_space) != "HTTP/1.1") {
        error = first_space != std::string_view::npos &&
                        line.substr(0, first_space) == "HTTP/1.0"
                    ? Http1ResponseParseStatus::VersionNotSupported
                    : Http1ResponseParseStatus::BadResponse;
        return std::nullopt;
    }
    const auto rest = line.substr(first_space + 1);
    const auto second_space = rest.find(' ');
    const auto code = rest.substr(0, second_space);
    if (code.size() != 3 ||
        !std::all_of(code.begin(), code.end(), [](char value) {
            return value >= '0' && value <= '9';
        })) {
        error = Http1ResponseParseStatus::BadResponse;
        return std::nullopt;
    }
    const int status = (code[0] - '0') * 100 + (code[1] - '0') * 10 +
        (code[2] - '0');
    return status;
}

/// 解析头块(状态行 + 头 + 结束 CRLFCRLF);失败时设置 error 并返回空。
std::optional<HeadFields> parse_head(
    std::string_view head,
    std::size_t max_body_bytes,
    Http1ResponseParseStatus& error) {
    HeadFields fields;
    const auto status_line_end = head.find("\r\n");
    if (status_line_end == std::string_view::npos) {
        error = Http1ResponseParseStatus::BadResponse;
        return std::nullopt;
    }
    const auto status =
        parse_status_line(head.substr(0, status_line_end), error);
    if (!status.has_value()) {
        return std::nullopt;
    }
    fields.response.status = *status;

    std::size_t content_length_count = 0;
    auto rest = head.substr(status_line_end + 2);
    while (!rest.empty()) {
        const auto line_end = rest.find("\r\n");
        if (line_end == std::string_view::npos) {
            error = Http1ResponseParseStatus::BadResponse;  // 头块未以空行收尾
            return std::nullopt;
        }
        const auto line = rest.substr(0, line_end);
        rest = rest.substr(line_end + 2);
        if (line.empty()) {
            break;  // 结束空行
        }
        if (line.front() == ' ' || line.front() == '\t') {
            error = Http1ResponseParseStatus::BadResponse;  // obs-fold
            return std::nullopt;
        }
        const auto colon = line.find(':');
        if (colon == std::string_view::npos || colon == 0 ||
            line[colon - 1] == ' ' || line[colon - 1] == '\t') {
            error = Http1ResponseParseStatus::BadResponse;
            return std::nullopt;
        }
        const auto name = line.substr(0, colon);
        if (!std::all_of(name.begin(), name.end(), is_tchar)) {
            error = Http1ResponseParseStatus::BadResponse;
            return std::nullopt;
        }
        auto value = trim_sp_htab(line.substr(colon + 1));
        if (!std::all_of(value.begin(), value.end(), is_valid_value_char)) {
            error = Http1ResponseParseStatus::BadResponse;
            return std::nullopt;
        }
        const auto normalized = lowercase(name);
        if (normalized == "transfer-encoding") {
            // v1 无 chunked:同时封死 chunked 响应与 TE+CL 走私两类路径。
            error = Http1ResponseParseStatus::BadResponse;
            return std::nullopt;
        }
        if (normalized == "content-length") {
            ++content_length_count;
            if (value.empty() || value.size() > std::to_string(
                                     std::numeric_limits<std::uint64_t>::max())
                                     .size()) {
                error = Http1ResponseParseStatus::BadResponse;
                return std::nullopt;
            }
            std::uint64_t parsed = 0;
            for (const char digit : value) {
                if (digit < '0' || digit > '9') {
                    error = Http1ResponseParseStatus::BadResponse;
                    return std::nullopt;
                }
                const auto numeric =
                    static_cast<std::uint64_t>(digit - '0');
                if (parsed >
                    (std::numeric_limits<std::uint64_t>::max() - numeric) /
                        10U) {
                    error = Http1ResponseParseStatus::BadResponse;  // 溢出
                    return std::nullopt;
                }
                parsed = parsed * 10U + numeric;
            }
            fields.content_length = parsed;
        }
        fields.response.headers.emplace_back(
            std::move(normalized), std::string{value});
    }
    if (content_length_count > 1) {
        error = Http1ResponseParseStatus::BadResponse;  // 重复 Content-Length
        return std::nullopt;
    }
    if (fields.content_length > max_body_bytes) {
        error = Http1ResponseParseStatus::PayloadTooLarge;
        return std::nullopt;
    }
    fields.response.body.reserve(
        static_cast<std::size_t>(fields.content_length));
    return fields;
}

}  // namespace

Http1ResponseParser::Http1ResponseParser(
    std::size_t max_head_bytes,
    std::size_t max_body_bytes)
    : max_head_bytes_(max_head_bytes),
      max_body_bytes_(max_body_bytes) {}

Http1ResponseParseResult Http1ResponseParser::try_parse(ByteBuffer& buffer) {
    if (phase_ == Phase::Body) {
        return consume_body(buffer);
    }
    const auto view = buffer.readable_data();
    const auto terminator = std::search(
        view.begin(), view.end(), head_terminator.begin(),
        head_terminator.end());
    if (terminator == view.end()) {
        // 头块未完但已超限:即时拒绝,慢速发送不绕开上限。
        if (view.size() > max_head_bytes_) {
            return {.status = Http1ResponseParseStatus::HeadersTooLarge};
        }
        return {.status = Http1ResponseParseStatus::NeedMoreData};
    }
    // 终止符在内才计入头块;body 字节随同一批到达不占用头预算。
    const auto head_size = static_cast<std::size_t>(
        std::distance(view.begin(), terminator)) +
        head_terminator.size();
    if (head_size > max_head_bytes_) {
        return {.status = Http1ResponseParseStatus::HeadersTooLarge};
    }
    auto error_status = Http1ResponseParseStatus::BadResponse;
    auto fields = parse_head(
        as_string_view(view.subspan(0, head_size)),
        max_body_bytes_,
        error_status);
    if (!fields.has_value()) {
        return {.status = error_status};
    }
    buffer.consume(head_size);
    if (fields->content_length == 0U) {
        return {
            .status = Http1ResponseParseStatus::ResponseReady,
            .response = std::move(fields->response)};
    }
    phase_ = Phase::Body;
    body_remaining_ = fields->content_length;
    pending_response_ = std::move(fields->response);
    // body 可能已随同一批字节到达:立即尝试消费,而非等下一轮投喂。
    return consume_body(buffer);
}

Http1ResponseParseResult Http1ResponseParser::consume_body(
    ByteBuffer& buffer) {
    const auto view = buffer.readable_data();
    const auto take =
        std::min<std::uint64_t>(view.size(), body_remaining_);
    pending_response_.body.append(
        reinterpret_cast<const char*>(view.data()),
        static_cast<std::size_t>(take));
    buffer.consume(static_cast<std::size_t>(take));
    body_remaining_ -= take;
    if (body_remaining_ > 0U) {
        return {.status = Http1ResponseParseStatus::NeedMoreData};
    }
    phase_ = Phase::Head;
    return {
        .status = Http1ResponseParseStatus::ResponseReady,
        .response = std::move(pending_response_)};
}

std::size_t Http1ResponseParser::max_input_bytes() const noexcept {
    return max_head_bytes_ + max_body_bytes_;
}

}  // namespace realm::network
