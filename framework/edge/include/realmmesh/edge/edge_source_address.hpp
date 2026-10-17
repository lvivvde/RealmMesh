#pragma once

// 来源地址归一化：直连 peer、可信代理 CIDR 与 X-Forwarded-For / Forwarded
// 解析。EdgeRuntime 在连接打开与对端地址变化时用它得到 Edge Connection 的
// 来源地址。
#include "realmmesh/network/transport/message_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realm::edge {

inline constexpr std::size_t edge_trusted_proxy_hard_max = 64;

enum class EdgeSourceMode : std::uint8_t {
    DirectPeer,
    TrustedXForwardedFor,
    TrustedForwarded,
};

[[nodiscard]] EdgeSourceMode parse_edge_source_mode(
    std::string_view value);
[[nodiscard]] std::string_view to_string(EdgeSourceMode mode) noexcept;

struct EdgeSourceConfig final {
    EdgeSourceMode mode{EdgeSourceMode::DirectPeer};
    std::vector<std::string> trusted_proxy_cidrs;

    void validate() const;
};

/// Runtime ingress 的单一来源规范化模块。转发头仅在 direct peer 命中
/// 显式 trusted proxy CIDR 时参与解析；否则无条件使用 direct peer。
class EdgeSourceNormalizer final {
public:
    explicit EdgeSourceNormalizer(EdgeSourceConfig config);
    ~EdgeSourceNormalizer();
    EdgeSourceNormalizer(EdgeSourceNormalizer&&) noexcept;
    EdgeSourceNormalizer& operator=(EdgeSourceNormalizer&&) noexcept;
    EdgeSourceNormalizer(const EdgeSourceNormalizer&) = delete;
    EdgeSourceNormalizer& operator=(const EdgeSourceNormalizer&) = delete;

    [[nodiscard]] std::optional<std::string> normalize(
        const network::TransportIngressSource& source) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::edge
