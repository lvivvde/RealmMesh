#pragma once

#include "realmmesh/cluster/etcd_service_registry.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace realm::game::gateway {

inline constexpr std::size_t admission_consumption_digest_key_size = 32;
using AdmissionConsumptionDigestKey =
    std::array<std::byte, admission_consumption_digest_key_size>;

[[nodiscard]] AdmissionConsumptionDigestKey
parse_admission_consumption_digest_key(std::string_view value);

struct AdmissionConsumptionOptions final {
    std::string key_prefix{"/realmmesh/admission/consumption"};
    std::chrono::seconds reservation_ttl{10};
    AdmissionConsumptionDigestKey digest_key{};

    void validate() const;
};

struct AdmissionReserveRequest final {
    std::string identity_jti;
    std::string grant_jti;
    std::string owner;
    std::chrono::system_clock::time_point now;
    /// Consumed 至少保留到整条凭据链无法再通过验证（含 clock leeway）。
    std::chrono::system_clock::time_point consume_until;
};

struct AdmissionReservation final {
    std::string identity_jti;
    std::string grant_jti;
    std::string owner;
    std::uint64_t fencing{0};
    std::chrono::system_clock::time_point lease_expires_at;
    std::chrono::system_clock::time_point consume_until;

    bool operator==(const AdmissionReservation&) const = default;
};

enum class AdmissionReserveStatus : std::uint8_t {
    Reserved,
    InProgress,
    Consumed,
    Unavailable,
};

struct AdmissionReserveResult final {
    AdmissionReserveStatus status{AdmissionReserveStatus::Unavailable};
    std::optional<AdmissionReservation> reservation;
};

enum class AdmissionMutationStatus : std::uint8_t {
    Applied,
    Lost,
    Unavailable,
};

/// Gateway Login Pipeline 的内部存储 seam。Pipeline 只保留一个外部
/// advance() 接口；reserve/commit/release 的两阶段顺序不会泄漏给
/// ServiceFrame 或传输层。
class AdmissionConsumptionStore {
public:
    virtual ~AdmissionConsumptionStore() = default;

    [[nodiscard]] virtual AdmissionReserveResult reserve(
        const AdmissionReserveRequest& request) = 0;
    [[nodiscard]] virtual AdmissionMutationStatus commit(
        const AdmissionReservation& reservation,
        std::chrono::system_clock::time_point now) = 0;
    [[nodiscard]] virtual AdmissionMutationStatus release(
        const AdmissionReservation& reservation,
        std::chrono::system_clock::time_point now) = 0;
    [[nodiscard]] virtual bool available() const noexcept = 0;
};

/// 确定性内存适配器。多 Pipeline 实例共享同一对象即可验证集群语义；
/// 生产装配不得用它替代不可用的 etcd。
class InMemoryAdmissionConsumptionStore final
    : public AdmissionConsumptionStore {
public:
    explicit InMemoryAdmissionConsumptionStore(
        AdmissionConsumptionOptions options);
    ~InMemoryAdmissionConsumptionStore() override;

    InMemoryAdmissionConsumptionStore(
        const InMemoryAdmissionConsumptionStore&) = delete;
    InMemoryAdmissionConsumptionStore& operator=(
        const InMemoryAdmissionConsumptionStore&) = delete;

    [[nodiscard]] AdmissionReserveResult reserve(
        const AdmissionReserveRequest& request) override;
    [[nodiscard]] AdmissionMutationStatus commit(
        const AdmissionReservation& reservation,
        std::chrono::system_clock::time_point now) override;
    [[nodiscard]] AdmissionMutationStatus release(
        const AdmissionReservation& reservation,
        std::chrono::system_clock::time_point now) override;
    [[nodiscard]] bool available() const noexcept override;

    void set_available(bool value) noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// etcd v3 线性一致适配器：range 使用默认 linearizable 读取，所有状态
/// 变化以 mod_revision CAS transaction 提交，并以覆盖完整凭据验证窗口的
/// etcd lease 自动清理。identity/grant jti 仅以部署密钥派生的稳定摘要
/// 出现在 key/value 中。
class EtcdAdmissionConsumptionStore final
    : public AdmissionConsumptionStore {
public:
    EtcdAdmissionConsumptionStore(
        AdmissionConsumptionOptions options,
        std::shared_ptr<cluster::IEtcdHttpClient> client);
    ~EtcdAdmissionConsumptionStore() override;

    EtcdAdmissionConsumptionStore(
        const EtcdAdmissionConsumptionStore&) = delete;
    EtcdAdmissionConsumptionStore& operator=(
        const EtcdAdmissionConsumptionStore&) = delete;

    [[nodiscard]] AdmissionReserveResult reserve(
        const AdmissionReserveRequest& request) override;
    [[nodiscard]] AdmissionMutationStatus commit(
        const AdmissionReservation& reservation,
        std::chrono::system_clock::time_point now) override;
    [[nodiscard]] AdmissionMutationStatus release(
        const AdmissionReservation& reservation,
        std::chrono::system_clock::time_point now) override;
    [[nodiscard]] bool available() const noexcept override;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::game::gateway
