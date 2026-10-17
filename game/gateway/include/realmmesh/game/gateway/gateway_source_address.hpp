#pragma once

// 来源地址归一化(ADR-0014 第 2 步):直连 peer、可信代理 CIDR 与
// X-Forwarded-For / Forwarded 解析。与游戏无关，只依赖 framework 与标准库;
// 连接运行时只包含本头。凭据入口见 gateway_credential_ingress.hpp。
#include "realmmesh/network/transport/message_transport.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace realm::game::gateway {

inline constexpr std::size_t gateway_trusted_proxy_hard_max = 64;

enum class GatewaySourceMode : std::uint8_t {
    DirectPeer,
    TrustedXForwardedFor,
    TrustedForwarded,
};

[[nodiscard]] GatewaySourceMode parse_gateway_source_mode(
    std::string_view value);
[[nodiscard]] std::string_view to_string(GatewaySourceMode mode) noexcept;

struct GatewaySourceConfig final {
    GatewaySourceMode mode{GatewaySourceMode::DirectPeer};
    std::vector<std::string> trusted_proxy_cidrs;

    void validate() const;
};

/// Runtime ingress 的单一来源规范化模块。转发头仅在 direct peer 命中
/// 显式 trusted proxy CIDR 时参与解析；否则无条件使用 direct peer。
class GatewaySourceNormalizer final {
public:
    explicit GatewaySourceNormalizer(GatewaySourceConfig config);
    ~GatewaySourceNormalizer();
    GatewaySourceNormalizer(GatewaySourceNormalizer&&) noexcept;
    GatewaySourceNormalizer& operator=(GatewaySourceNormalizer&&) noexcept;
    GatewaySourceNormalizer(const GatewaySourceNormalizer&) = delete;
    GatewaySourceNormalizer& operator=(const GatewaySourceNormalizer&) = delete;

    [[nodiscard]] std::optional<std::string> normalize(
        const network::TransportIngressSource& source) const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::game::gateway
