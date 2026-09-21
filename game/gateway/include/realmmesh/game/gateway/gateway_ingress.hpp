#pragma once

#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/transport/message_transport.hpp"

#include <chrono>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace realm::game::gateway {

inline constexpr std::size_t gateway_attach_envelope_hard_max = 16'384;
inline constexpr std::size_t gateway_token_hard_max = 4'096;
inline constexpr std::size_t gateway_token_decoded_hard_max = 3'072;
inline constexpr std::uint32_t gateway_source_rate_hard_max = 10'000;
inline constexpr std::uint32_t gateway_source_burst_hard_max = 20'000;
inline constexpr std::uint32_t gateway_session_attempt_hard_max = 16;
inline constexpr std::uint32_t gateway_verification_hard_max = 4'096;
inline constexpr std::size_t gateway_trusted_proxy_hard_max = 64;
inline constexpr std::size_t gateway_tracked_source_hard_max = 65'536;

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

struct GatewayCredentialIngressConfig final {
    std::size_t max_attach_envelope_bytes{gateway_attach_envelope_hard_max};
    std::size_t max_identity_token_bytes{2'048};
    std::size_t max_admission_grant_bytes{2'048};
    std::size_t max_token_decoded_bytes{gateway_token_decoded_hard_max};
    std::uint32_t source_rate_per_second{20};
    std::uint32_t source_burst{40};
    std::uint32_t source_throttle_close_after{3};
    std::uint32_t session_attach_attempts{4};
    std::uint32_t concurrent_verifications{128};
    std::size_t max_tracked_sources{16'384};

    void validate() const;
};

enum class GatewayIngressStatus : std::uint8_t {
    Allowed,
    Oversized,
    Malformed,
    SourceThrottled,
    SustainedAbuse,
    SessionLimited,
    VerificationSaturated,
};

class GatewayVerificationLease final {
public:
    GatewayVerificationLease() = default;
    ~GatewayVerificationLease();
    GatewayVerificationLease(GatewayVerificationLease&& other) noexcept;
    GatewayVerificationLease& operator=(
        GatewayVerificationLease&& other) noexcept;
    GatewayVerificationLease(const GatewayVerificationLease&) = delete;
    GatewayVerificationLease& operator=(const GatewayVerificationLease&) =
        delete;

private:
    friend class GatewayCredentialIngress;
    explicit GatewayVerificationLease(std::atomic_uint64_t* counter) noexcept;
    std::atomic_uint64_t* counter_{nullptr};
};

struct GatewayIngressCheck final {
    GatewayIngressStatus status{GatewayIngressStatus::Malformed};
    std::optional<common::EdgeAttach> attach;
    GatewayVerificationLease verification;
    std::chrono::seconds retry_after{0};
};

struct GatewayIngressCounters final {
    std::uint64_t oversized{0};
    std::uint64_t malformed{0};
    std::uint64_t source_throttled{0};
    std::uint64_t sustained_abuse{0};
    std::uint64_t session_limited{0};
    std::uint64_t verification_saturated{0};
    std::uint64_t verification_in_flight{0};
};

/// Pipeline 的内部入口守卫。成功返回已做 bounded protobuf/JWS 结构检查的
/// Attach 和一个 RAII 验签额度；失败在任何 Ed25519 工作之前完成。
class GatewayCredentialIngress final {
public:
    explicit GatewayCredentialIngress(GatewayCredentialIngressConfig config);
    ~GatewayCredentialIngress();
    GatewayCredentialIngress(GatewayCredentialIngress&&) noexcept;
    GatewayCredentialIngress& operator=(GatewayCredentialIngress&&) noexcept;
    GatewayCredentialIngress(const GatewayCredentialIngress&) = delete;
    GatewayCredentialIngress& operator=(const GatewayCredentialIngress&) =
        delete;

    [[nodiscard]] GatewayIngressCheck inspect_attach(
        std::string_view normalized_source,
        std::uint32_t session_attempt,
        std::span<const std::byte> payload,
        std::chrono::steady_clock::time_point now);

    [[nodiscard]] GatewayIngressCounters counters() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::game::gateway
