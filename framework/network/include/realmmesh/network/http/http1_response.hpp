#pragma once

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realm::network {

struct Http1Response final {
    int status{200};
    /// 以给定顺序输出;Content-Length 与 Connection 由序列化器补齐。
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// 大小写不敏感按名取头(名已小写归一);缺失返回 nullptr。
    [[nodiscard]] const std::string* header(std::string_view name) const;
};

/// 恒写 Content-Length(body 字节数)与 Connection;已知状态码配
/// reason phrase,未知状态码省略短语。纯函数,与解析器同型可单测。
[[nodiscard]] std::string serialize_http1_response(
    const Http1Response& response,
    bool keep_alive);

}  // namespace realm::network
