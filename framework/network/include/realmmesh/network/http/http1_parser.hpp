#pragma once

#include "realmmesh/network/core/byte_buffer.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realm::network {

enum class Http1ParseStatus {
    NeedMoreData,
    RequestReady,
    /// 语法与走私类违规(Transfer-Encoding 出现、重复/缺失 Host、畸形
    /// 请求行或头)→ 400。
    BadRequest,
    /// body 超限 → 413。
    PayloadTooLarge,
    /// 请求行+头块超限 → 431。
    HeadersTooLarge,
    /// 非 HTTP/1.1 → 505。
    VersionNotSupported,
};

struct Http1Request final {
    std::string method;
    /// origin-form 目标(/path?query)。
    std::string target;
    /// 名已小写归一,保序存储;重复名不合并,语义由路由层处置。
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;

    /// 大小写不敏感按名取头;缺失返回 nullptr。
    [[nodiscard]] const std::string* header(std::string_view name) const;
    /// RFC 9112 §9.3:HTTP/1.1 缺省 keep-alive,Connection 含 close 即关闭。
    [[nodiscard]] bool wants_keep_alive() const;
};

struct Http1ParseResult final {
    Http1ParseStatus status{Http1ParseStatus::NeedMoreData};
    std::optional<Http1Request> request;
};

/// HTTP/1.1 有界请求解析器(ADR-0007):逐块投喂,一次最多产出一个完整
/// 请求,流水线剩余字节留在原缓冲。解析是确定性拒绝——同一字节序列永远
/// 得到同一结论;错误后调用方不得继续使用同一解析器(连接应关闭)。
class Http1Parser final {
public:
    static constexpr std::size_t default_max_head_bytes = 8 * 1024;
    static constexpr std::size_t default_max_body_bytes = 16 * 1024;

    Http1Parser(
        std::size_t max_head_bytes = default_max_head_bytes,
        std::size_t max_body_bytes = default_max_body_bytes);

    /// 从 buffer 消费至多一个请求。
    [[nodiscard]] Http1ParseResult try_parse(ByteBuffer& buffer);

    /// 单请求总投入上限(头块 + body);服务边以此约束每连接输入缓冲。
    [[nodiscard]] std::size_t max_input_bytes() const noexcept;

private:
    enum class Phase { Head, Body };

    [[nodiscard]] Http1ParseResult consume_body(ByteBuffer& buffer);

    Phase phase_{Phase::Head};
    std::size_t max_head_bytes_;
    std::size_t max_body_bytes_;
    std::uint64_t body_remaining_{0};
    Http1Request pending_request_;
};

}  // namespace realm::network
