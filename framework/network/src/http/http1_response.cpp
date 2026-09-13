#include "realmmesh/network/http/http1_response.hpp"

#include "http1_lexing.hpp"

#include <algorithm>
#include <string_view>

namespace realm::network {
namespace {

// 词法助手收敛在 http1_lexing.hpp(请求侧/响应侧共用)。
using lexing::equals_ignore_case;

std::string_view reason_phrase(int status) {
    switch (status) {
    case 200: return "OK";
    case 202: return "Accepted";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Content Too Large";
    case 429: return "Too Many Requests";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 505: return "HTTP Version Not Supported";
    default: return "";
    }
}

}  // namespace

const std::string* Http1Response::header(std::string_view name) const {
    for (const auto& [key, value] : headers) {
        if (equals_ignore_case(key, name)) {
            return &value;
        }
    }
    return nullptr;
}

std::string serialize_http1_response(
    const Http1Response& response,
    bool keep_alive) {
    std::string serialized;
    serialized.reserve(128 + response.body.size());
    serialized += "HTTP/1.1 ";
    serialized += std::to_string(response.status);
    const auto phrase = reason_phrase(response.status);
    if (!phrase.empty()) {
        serialized += ' ';
        serialized += phrase;
    }
    serialized += "\r\n";
    for (const auto& [name, value] : response.headers) {
        serialized += name;
        serialized += ": ";
        serialized += value;
        serialized += "\r\n";
    }
    serialized += "Content-Length: ";
    serialized += std::to_string(response.body.size());
    serialized += "\r\n";
    serialized += keep_alive ? "Connection: keep-alive\r\n"
                             : "Connection: close\r\n";
    serialized += "\r\n";
    serialized += response.body;
    return serialized;
}

}  // namespace realm::network
