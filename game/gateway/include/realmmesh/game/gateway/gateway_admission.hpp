#pragma once

#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/gateway/admission_consumption_store.hpp"
#include "realmmesh/game/gateway/gateway_primary_transport.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace realm::game::gateway {

enum class GatewayAdmissionStartStatus : std::uint8_t {
    Reserved,
    InvalidCredentials,
    InProgress,
    Consumed,
    StoreUnavailable,
};

/// Pipeline 持有的不透明预留。调用方只能取得 account_id；身份、Grant、
/// fencing 与保留期留在 GatewayAdmission 内部，避免存储协议扩散。
class GatewayAdmissionReservation final {
public:
    GatewayAdmissionReservation(GatewayAdmissionReservation&&) noexcept =
        default;
    GatewayAdmissionReservation& operator=(
        GatewayAdmissionReservation&&) noexcept = default;
    GatewayAdmissionReservation(const GatewayAdmissionReservation&) = delete;
    GatewayAdmissionReservation& operator=(
        const GatewayAdmissionReservation&) = delete;

    [[nodiscard]] std::uint64_t account_id() const noexcept;

private:
    friend class GatewayAdmission;

    GatewayAdmissionReservation(
        std::uint64_t account_id, AdmissionReservation reservation);

    std::uint64_t account_id_{0};
    AdmissionReservation reservation_;
    bool terminal_{false};
};

struct GatewayAdmissionStartResult final {
    GatewayAdmissionStartStatus status{
        GatewayAdmissionStartStatus::InvalidCredentials};
    std::optional<GatewayAdmissionReservation> reservation;
};

enum class GatewayAdmissionTransitionStatus : std::uint8_t {
    Pending,
    Applied,
    Lost,
    StoreUnavailable,
};

/// Gateway Login Pipeline 的私有凭据/消费模块。调用方提交两种凭据与
/// bounded owner，模块在分布式预留前完成 schema、签名、时间、部署和
/// identity_jti 绑定验证；存储错误不会降级为进程内 replay guard。
class GatewayAdmission final {
public:
    GatewayAdmission(
        common::IdentityTokenCodec identity_tokens,
        std::string identity_issuer,
        common::AdmissionGrantVerifier admission_grants,
        AdmissionConsumptionStore& store);

    [[nodiscard]] GatewayAdmissionStartResult reserve(
        std::string_view identity_token,
        std::string_view admission_grant,
        std::string owner,
        std::chrono::system_clock::time_point now);

    /// 吞掉 Primary Transport 的三态结果并保持提交顺序私有：Full 不改
    /// 存储且保留 reservation；Queued 才提交；Stopped 在 precommit
    /// 释放。Queued 后任何 Unavailable 都按 postcommit ambiguity 处理：
    /// reservation 终结且绝不补 release。
    [[nodiscard]] GatewayAdmissionTransitionStatus on_accept_result(
        GatewayAdmissionReservation& reservation,
        PrimaryTransportResult result,
        std::chrono::system_clock::time_point now);

    /// precommit SessionClosed 或放弃尝试时调用；owner/fencing 不匹配时
    /// 返回 Lost，不能释放后来者的 reservation。
    [[nodiscard]] GatewayAdmissionTransitionStatus abandon(
        GatewayAdmissionReservation& reservation,
        std::chrono::system_clock::time_point now);

    [[nodiscard]] bool available() const noexcept;
    [[nodiscard]] bool refresh_availability(
        std::chrono::steady_clock::time_point now);

private:
    common::IdentityTokenCodec identity_tokens_;
    std::string identity_issuer_;
    common::AdmissionGrantVerifier admission_grants_;
    AdmissionConsumptionStore* store_;
    std::optional<std::chrono::steady_clock::time_point> next_probe_;
};

}  // namespace realm::game::gateway
