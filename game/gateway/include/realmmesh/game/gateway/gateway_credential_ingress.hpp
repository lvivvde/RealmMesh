#pragma once

// 凭据入口(ADR-0014 第 2 步):登录管线解码 EdgeAttach 前的有界检查、
// 来源令牌桶与验签额度。输入是已归一化的来源字符串，来源地址归一化见
// gateway_source_address.hpp。
#include "realmmesh/game/common/edge_protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>

namespace realm::game::gateway {

inline constexpr std::size_t gateway_attach_envelope_hard_max = 16'384;
inline constexpr std::size_t gateway_token_hard_max = 4'096;
inline constexpr std::size_t gateway_token_decoded_hard_max = 3'072;
inline constexpr std::uint32_t gateway_source_rate_hard_max = 10'000;
inline constexpr std::uint32_t gateway_source_burst_hard_max = 20'000;
inline constexpr std::uint32_t gateway_session_attempt_hard_max = 16;
inline constexpr std::uint32_t gateway_verification_hard_max = 4'096;
inline constexpr std::size_t gateway_tracked_source_hard_max = 65'536;

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
