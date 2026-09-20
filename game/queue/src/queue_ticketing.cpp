#include "realmmesh/game/queue/queue_ticketing.hpp"

#include <sodium.h>

#include <array>
#include <stdexcept>
#include <utility>

namespace realm::game::queue {
namespace {

std::uint64_t estimate_wait(std::uint64_t position, std::uint64_t rate) {
    if (position == 0 || rate == 0) return 0;
    return (position + rate - 1U) / rate;
}

std::string deterministic_grant_jti(
    std::string_view identity_jti,
    std::uint64_t queue_number,
    std::chrono::system_clock::time_point released_at) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    std::string input{"realmmesh/admission-grant-jti/v1"};
    input.push_back('\0');
    input.append(identity_jti);
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.push_back(static_cast<char>((queue_number >> shift) & 0xFFU));
    }
    const auto released_seconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            released_at.time_since_epoch())
            .count();
    const auto released_bits = static_cast<std::uint64_t>(released_seconds);
    for (int shift = 56; shift >= 0; shift -= 8) {
        input.push_back(static_cast<char>((released_bits >> shift) & 0xFFU));
    }

    std::array<unsigned char, 16> digest{};
    if (crypto_generichash(
            digest.data(), digest.size(),
            reinterpret_cast<const unsigned char*>(input.data()),
            input.size(), nullptr, 0) != 0) {
        throw std::runtime_error("failed to derive Admission Grant jti");
    }
    static constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(digest.size() * 2U);
    for (const auto byte : digest) {
        result.push_back(hex[byte >> 4U]);
        result.push_back(hex[byte & 0x0FU]);
    }
    return result;
}

}  // namespace

QueueTicketing::QueueTicketing(
    QueueCore& core,
    common::QueueNumberV2Codec queue_numbers,
    common::AdmissionGrantIssuer admission_grants)
    : core_(&core),
      queue_numbers_(std::move(queue_numbers)),
      admission_grants_(std::move(admission_grants)) {}

IssuedQueueTicket QueueTicketing::issue(
    const common::IdentityClaims& identity,
    std::chrono::system_clock::time_point now) {
    const auto issued = core_->issue(identity.jti, now);
    const auto token = queue_numbers_.issue(common::QueueNumberV2Issue{
        .identity_jti = identity.jti,
        .number = issued.number,
        .issued_at = now,
        .identity_expires_at = identity.expires_at,
    });
    const auto released = core_->released_number();
    const auto position =
        issued.number > released ? issued.number - released : 0U;
    return {
        .queue_number_token = token,
        .number = issued.number,
        .estimated_wait_seconds =
            estimate_wait(position, core_->admit_rate(now)),
    };
}

QueueTicketQuery QueueTicketing::query(
    std::string_view queue_number_token,
    std::chrono::system_clock::time_point now) const {
    const auto claims = queue_numbers_.validate(queue_number_token, now);
    if (!claims.has_value()) {
        return {.status = QueueTicketQueryStatus::InvalidQueueNumber};
    }

    const auto eligibility =
        core_->release_eligibility(claims->number, now);
    if (eligibility.status == QueueReleaseStatus::NotReleased) {
        const auto position = claims->number - core_->released_number();
        return {
            .status = QueueTicketQueryStatus::Queued,
            .number = claims->number,
            .position = position,
            .estimated_wait_seconds =
                estimate_wait(position, core_->admit_rate(now)),
        };
    }
    if (eligibility.status == QueueReleaseStatus::Expired ||
        claims->expires_at < eligibility.released_at) {
        return {
            .status = QueueTicketQueryStatus::ReleaseExpired,
            .number = claims->number,
        };
    }

    auto grant = admission_grants_.issue(common::AdmissionGrantIssue{
        .grant_jti = deterministic_grant_jti(
            claims->identity_jti, claims->number, eligibility.released_at),
        .identity_jti = claims->identity_jti,
        .queue_number = claims->number,
        .released_at = eligibility.released_at,
        .issued_at = eligibility.released_at,
        .identity_expires_at = claims->expires_at,
    });
    return {
        .status = QueueTicketQueryStatus::Admitted,
        .number = claims->number,
        .admission_grant = std::move(grant),
    };
}

}  // namespace realm::game::queue
