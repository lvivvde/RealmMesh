#pragma once

#include "realmmesh/network/core/byte_buffer.hpp"
#include "realmmesh/network/http/http1_response.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>

namespace realm::network {

enum class Http1ResponseParseStatus {
    NeedMoreData,
    ResponseReady,
    /// 状态行/头语法畸形、Transfer-Encoding 出现、重复 Content-Length。
    BadResponse,
    /// body 超限。
    PayloadTooLarge,
    /// 状态行+头块超限。
    HeadersTooLarge,
    /// 非 HTTP/1.1。
    VersionNotSupported,
};

struct Http1ResponseParseResult final {
    Http1ResponseParseStatus status{Http1ResponseParseStatus::NeedMoreData};
    std::optional<Http1Response> response;
};

/// HTTP/1.1 有界响应解析器(客户端边,ADR-0007 最小补齐):逐块投喂,
/// 一次至多产出一个完整响应,流水线剩余字节留在原缓冲。v1 无 chunked
/// (Transfer-Encoding 一律拒绝);无 Content-Length 视为空 body。错误
/// 后调用方不得继续使用同一解析器(连接应关闭)。
class Http1ResponseParser final {
public:
    static constexpr std::size_t default_max_head_bytes = 8 * 1024;
    static constexpr std::size_t default_max_body_bytes = 1024 * 1024;

    Http1ResponseParser(
        std::size_t max_head_bytes = default_max_head_bytes,
        std::size_t max_body_bytes = default_max_body_bytes);

    /// 从 buffer 消费至多一个响应。
    [[nodiscard]] Http1ResponseParseResult try_parse(ByteBuffer& buffer);

private:
    enum class Phase { Head, Body };

    [[nodiscard]] Http1ResponseParseResult consume_body(ByteBuffer& buffer);

    Phase phase_{Phase::Head};
    std::size_t max_head_bytes_;
    std::size_t max_body_bytes_;
    std::uint64_t body_remaining_{0};
    Http1Response pending_response_;
};

}  // namespace realm::network
