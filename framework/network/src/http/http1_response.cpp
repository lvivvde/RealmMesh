#include "realmmesh/network/http/http1_response.hpp"

#include <algorithm>
#include <string_view>

namespace realm::network {
namespace {

char to_lower(char value) {
    return value >= 'A' && value <= 'Z'
               ? static_cast<char>(value - 'A' + 'a')
               : value;
}

bool equals_ignore_case(std::string_view left, std::string_view right) {
    return left.size() == right.size() && std::equal(
                                              left.begin(),
                                              left.end(),
                                              right.begin(),
                                              [](char lhs, char rhs) {
                                                  return to_lower(lhs) ==
                                                      to_lower(rhs);
                                              });
}

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
