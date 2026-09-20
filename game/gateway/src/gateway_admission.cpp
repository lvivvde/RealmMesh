#include "realmmesh/game/gateway/gateway_admission.hpp"

#include "realmmesh/game/common/compact_jws.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace realm::game::gateway {

GatewayAdmissionReservation::GatewayAdmissionReservation(
    std::uint64_t account_id, AdmissionReservation reservation)
    : account_id_(account_id), reservation_(std::move(reservation)) {}

std::uint64_t GatewayAdmissionReservation::account_id() const noexcept {
    return account_id_;
}

GatewayAdmission::GatewayAdmission(
    common::IdentityTokenCodec identity_tokens,
    std::string identity_issuer,
    common::AdmissionGrantVerifier admission_grants,
    AdmissionConsumptionStore& store)
    : identity_tokens_(std::move(identity_tokens)),
      identity_issuer_(std::move(identity_issuer)),
      admission_grants_(std::move(admission_grants)),
      store_(&store) {
    if (identity_issuer_.empty()) {
        throw std::invalid_argument("identity issuer must not be empty");
    }
}

GatewayAdmissionStartResult GatewayAdmission::reserve(
    std::string_view identity_token,
    std::string_view admission_grant,
    std::string owner,
    std::chrono::system_clock::time_point now) {
    const auto identity =
        identity_tokens_.validate(identity_token, identity_issuer_, now);
    if (!identity.has_value()) {
        return {.status = GatewayAdmissionStartStatus::InvalidCredentials};
    }
    const auto grant = admission_grants_.validate(
        admission_grant, identity->jti, identity->expires_at, now);
    if (!grant.has_value()) {
        return {.status = GatewayAdmissionStartStatus::InvalidCredentials};
    }

    auto stored = store_->reserve(AdmissionReserveRequest{
        .identity_jti = identity->jti,
        .grant_jti = grant->grant_jti,
        .owner = std::move(owner),
        .now = now,
        .consume_until =
            std::min(identity->expires_at, grant->expires_at) +
            common::jws_clock_leeway,
    });
    switch (stored.status) {
    case AdmissionReserveStatus::Reserved:
        if (!stored.reservation.has_value()) {
            return {.status = GatewayAdmissionStartStatus::StoreUnavailable};
        }
        return {
            .status = GatewayAdmissionStartStatus::Reserved,
            .reservation = GatewayAdmissionReservation{
                identity->account_id, std::move(*stored.reservation)},
        };
    case AdmissionReserveStatus::InProgress:
        return {.status = GatewayAdmissionStartStatus::InProgress};
    case AdmissionReserveStatus::Consumed:
        return {.status = GatewayAdmissionStartStatus::Consumed};
    case AdmissionReserveStatus::Unavailable:
        return {.status = GatewayAdmissionStartStatus::StoreUnavailable};
    }
    return {.status = GatewayAdmissionStartStatus::StoreUnavailable};
}

namespace {

GatewayAdmissionTransitionStatus transition_status(
    AdmissionMutationStatus status) {
    switch (status) {
    case AdmissionMutationStatus::Applied:
        return GatewayAdmissionTransitionStatus::Applied;
    case AdmissionMutationStatus::Lost:
        return GatewayAdmissionTransitionStatus::Lost;
    case AdmissionMutationStatus::Unavailable:
        return GatewayAdmissionTransitionStatus::StoreUnavailable;
    }
    return GatewayAdmissionTransitionStatus::StoreUnavailable;
}

}  // namespace

GatewayAdmissionTransitionStatus GatewayAdmission::on_accept_result(
    GatewayAdmissionReservation& reservation,
    PrimaryTransportResult result,
    std::chrono::system_clock::time_point now) {
    if (reservation.terminal_) return GatewayAdmissionTransitionStatus::Lost;
    if (result == PrimaryTransportResult::Full) {
        return GatewayAdmissionTransitionStatus::Pending;
    }
    if (result == PrimaryTransportResult::Stopped) {
        reservation.terminal_ = true;
        return transition_status(store_->release(reservation.reservation_, now));
    }
    // 在触碰远端前先终结本地 handle。HTTP 超时无法证明 transaction 未
    // 提交，随后 release 会把已经 Consumed 的凭据重新打开。
    reservation.terminal_ = true;
    return transition_status(store_->commit(reservation.reservation_, now));
}

GatewayAdmissionTransitionStatus GatewayAdmission::abandon(
    GatewayAdmissionReservation& reservation,
    std::chrono::system_clock::time_point now) {
    if (reservation.terminal_) return GatewayAdmissionTransitionStatus::Lost;
    reservation.terminal_ = true;
    return transition_status(store_->release(reservation.reservation_, now));
}

bool GatewayAdmission::available() const noexcept {
    return store_->available();
}

}  // namespace realm::game::gateway
