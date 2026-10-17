#include "realmmesh/game/gateway/gateway_credential_ingress.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <mutex>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>

namespace realm::game::gateway {
namespace {

bool valid_base64url_segment(std::string_view segment) {
    return !segment.empty() &&
        std::ranges::all_of(segment, [](unsigned char character) {
            return std::isalnum(character) != 0 || character == '-' ||
                character == '_';
        });
}

std::size_t decoded_upper_bound(std::size_t encoded) {
    return (encoded / 4U) * 3U + ((encoded % 4U) * 3U + 3U) / 4U;
}

bool bounded_compact_jws(
    std::string_view token,
    std::size_t encoded_limit,
    std::size_t decoded_limit) {
    if (token.empty() || token.size() > encoded_limit) return false;
    const auto first = token.find('.');
    if (first == std::string_view::npos) return false;
    const auto second = token.find('.', first + 1U);
    if (second == std::string_view::npos ||
        token.find('.', second + 1U) != std::string_view::npos) {
        return false;
    }
    const std::array segments{
        token.substr(0, first),
        token.substr(first + 1U, second - first - 1U),
        token.substr(second + 1U),
    };
    if (segments[2].size() != 86U) return false;
    std::size_t decoded = 0;
    for (const auto segment : segments) {
        if (!valid_base64url_segment(segment)) return false;
        const auto addition = decoded_upper_bound(segment.size());
        if (addition > decoded_limit - std::min(decoded, decoded_limit)) {
            return false;
        }
        decoded += addition;
    }
    return decoded <= decoded_limit;
}

}  // namespace

void GatewayCredentialIngressConfig::validate() const {
    if (max_attach_envelope_bytes == 0 ||
        max_attach_envelope_bytes > gateway_attach_envelope_hard_max ||
        max_identity_token_bytes == 0 ||
        max_identity_token_bytes > gateway_token_hard_max ||
        max_admission_grant_bytes == 0 ||
        max_admission_grant_bytes > gateway_token_hard_max ||
        max_token_decoded_bytes == 0 ||
        max_token_decoded_bytes > gateway_token_decoded_hard_max ||
        source_rate_per_second == 0 ||
        source_rate_per_second > gateway_source_rate_hard_max ||
        source_burst == 0 || source_burst > gateway_source_burst_hard_max ||
        source_throttle_close_after == 0 ||
        source_throttle_close_after > gateway_session_attempt_hard_max ||
        session_attach_attempts == 0 ||
        session_attach_attempts > gateway_session_attempt_hard_max ||
        concurrent_verifications == 0 ||
        concurrent_verifications > gateway_verification_hard_max ||
        max_tracked_sources == 0 ||
        max_tracked_sources > gateway_tracked_source_hard_max) {
        throw std::invalid_argument(
            "gateway credential ingress setting exceeds protocol bounds");
    }
    if (source_burst < source_rate_per_second) {
        throw std::invalid_argument(
            "gateway source burst must be at least the per-second rate");
    }
}

namespace {

using VerificationCounter = std::atomic_uint64_t;

void release_counter(VerificationCounter*& counter) noexcept {
    if (counter == nullptr) return;
    counter->fetch_sub(1U);
    counter = nullptr;
}

}  // namespace

GatewayVerificationLease::GatewayVerificationLease(
    VerificationCounter* counter) noexcept
    : counter_(counter) {}
GatewayVerificationLease::~GatewayVerificationLease() {
    release_counter(counter_);
}
GatewayVerificationLease::GatewayVerificationLease(
    GatewayVerificationLease&& other) noexcept
    : counter_(std::exchange(other.counter_, nullptr)) {}
GatewayVerificationLease& GatewayVerificationLease::operator=(
    GatewayVerificationLease&& other) noexcept {
    if (this == &other) return *this;
    release_counter(counter_);
    counter_ = std::exchange(other.counter_, nullptr);
    return *this;
}

class GatewayCredentialIngress::Impl final {
public:
    explicit Impl(GatewayCredentialIngressConfig config)
        : config_(std::move(config)) {
        config_.validate();
    }

    GatewayIngressCheck inspect_attach(
        std::string_view source,
        std::uint32_t session_attempt,
        std::span<const std::byte> payload,
        std::chrono::steady_clock::time_point now) {
        if (source.empty()) return reject(GatewayIngressStatus::Malformed);
        if (payload.size() > config_.max_attach_envelope_bytes) {
            return reject(GatewayIngressStatus::Oversized);
        }
        const auto throttle = consume_source(source, now);
        if (throttle.status != GatewayIngressStatus::Allowed) {
            return reject(throttle.status, throttle.retry_after);
        }
        if (session_attempt > config_.session_attach_attempts) {
            return reject(GatewayIngressStatus::SessionLimited);
        }
        auto attach = common::decode_edge_attach(payload);
        if (!attach.has_value()) {
            return reject(GatewayIngressStatus::Malformed);
        }
        if (attach->identity_token().size() >
                config_.max_identity_token_bytes ||
            attach->admission_grant().size() >
                config_.max_admission_grant_bytes) {
            return reject(GatewayIngressStatus::Oversized);
        }
        if (!bounded_compact_jws(
                attach->identity_token(),
                config_.max_identity_token_bytes,
                config_.max_token_decoded_bytes) ||
            !bounded_compact_jws(
                attach->admission_grant(),
                config_.max_admission_grant_bytes,
                config_.max_token_decoded_bytes)) {
            return reject(GatewayIngressStatus::Malformed);
        }
        auto in_flight = verification_in_flight_.load();
        while (in_flight < config_.concurrent_verifications) {
            if (verification_in_flight_.compare_exchange_weak(
                    in_flight, in_flight + 1U)) {
                return {
                    .status = GatewayIngressStatus::Allowed,
                    .attach = std::move(attach),
                    .verification = GatewayVerificationLease{
                        &verification_in_flight_},
                    .retry_after = std::chrono::seconds::zero(),
                };
            }
        }
        return reject(
            GatewayIngressStatus::VerificationSaturated,
            std::chrono::seconds{1});
    }

    GatewayIngressCounters counters() const noexcept {
        return {
            .oversized = oversized_.load(),
            .malformed = malformed_.load(),
            .source_throttled = source_throttled_.load(),
            .sustained_abuse = sustained_abuse_.load(),
            .session_limited = session_limited_.load(),
            .verification_saturated = verification_saturated_.load(),
            .verification_in_flight = verification_in_flight_.load(),
        };
    }

private:
    struct SourceDecision final {
        GatewayIngressStatus status{GatewayIngressStatus::Allowed};
        std::chrono::seconds retry_after{0};
    };

    struct Bucket final {
        double tokens{0.0};
        std::chrono::steady_clock::time_point updated_at;
        std::chrono::steady_clock::time_point last_seen;
        std::uint32_t consecutive_throttles{0};
    };

    SourceDecision consume_source(
        std::string_view source,
        std::chrono::steady_clock::time_point now) {
        std::scoped_lock lock(buckets_mutex_);
        auto found = buckets_.find(std::string{source});
        if (found == buckets_.end()) {
            if (buckets_.size() >= config_.max_tracked_sources) {
                const auto oldest = std::ranges::min_element(
                    buckets_, {}, [](const auto& entry) {
                        return entry.second.last_seen;
                    });
                if (oldest != buckets_.end()) buckets_.erase(oldest);
            }
            found = buckets_
                        .emplace(
                            source,
                            Bucket{
                                .tokens =
                                    static_cast<double>(config_.source_burst),
                                .updated_at = now,
                                .last_seen = now,
                            })
                        .first;
        }
        auto& bucket = found->second;
        const auto elapsed = std::max(
            0.0,
            std::chrono::duration<double>(now - bucket.updated_at).count());
        bucket.tokens = std::min(
            static_cast<double>(config_.source_burst),
            bucket.tokens +
                elapsed * static_cast<double>(config_.source_rate_per_second));
        bucket.updated_at = now;
        bucket.last_seen = now;
        if (bucket.tokens >= 1.0) {
            bucket.tokens -= 1.0;
            bucket.consecutive_throttles = 0;
            return {.status = GatewayIngressStatus::Allowed};
        }
        ++bucket.consecutive_throttles;
        if (bucket.consecutive_throttles >=
            config_.source_throttle_close_after) {
            return {.status = GatewayIngressStatus::SustainedAbuse};
        }
        const auto seconds = std::max(
            1.0,
            std::ceil(
                (1.0 - bucket.tokens) /
                static_cast<double>(config_.source_rate_per_second)));
        return {
            .status = GatewayIngressStatus::SourceThrottled,
            .retry_after =
                std::chrono::seconds{static_cast<std::int64_t>(seconds)},
        };
    }

    GatewayIngressCheck reject(
        GatewayIngressStatus status,
        std::chrono::seconds retry_after = std::chrono::seconds::zero()) {
        switch (status) {
        case GatewayIngressStatus::Oversized:
            oversized_.fetch_add(1U);
            break;
        case GatewayIngressStatus::Malformed:
            malformed_.fetch_add(1U);
            break;
        case GatewayIngressStatus::SourceThrottled:
            source_throttled_.fetch_add(1U);
            break;
        case GatewayIngressStatus::SustainedAbuse:
            sustained_abuse_.fetch_add(1U);
            break;
        case GatewayIngressStatus::SessionLimited:
            session_limited_.fetch_add(1U);
            break;
        case GatewayIngressStatus::VerificationSaturated:
            verification_saturated_.fetch_add(1U);
            break;
        case GatewayIngressStatus::Allowed:
            break;
        }
        return {.status = status, .retry_after = retry_after};
    }

    GatewayCredentialIngressConfig config_;
    mutable std::mutex buckets_mutex_;
    std::unordered_map<std::string, Bucket> buckets_;
    VerificationCounter verification_in_flight_{0};
    std::atomic_uint64_t oversized_{0};
    std::atomic_uint64_t malformed_{0};
    std::atomic_uint64_t source_throttled_{0};
    std::atomic_uint64_t sustained_abuse_{0};
    std::atomic_uint64_t session_limited_{0};
    std::atomic_uint64_t verification_saturated_{0};
};

GatewayCredentialIngress::GatewayCredentialIngress(
    GatewayCredentialIngressConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
GatewayCredentialIngress::~GatewayCredentialIngress() = default;
GatewayCredentialIngress::GatewayCredentialIngress(
    GatewayCredentialIngress&&) noexcept = default;
GatewayCredentialIngress& GatewayCredentialIngress::operator=(
    GatewayCredentialIngress&&) noexcept = default;
GatewayIngressCheck GatewayCredentialIngress::inspect_attach(
    std::string_view normalized_source,
    std::uint32_t session_attempt,
    std::span<const std::byte> payload,
    std::chrono::steady_clock::time_point now) {
    return impl_->inspect_attach(
        normalized_source, session_attempt, payload, now);
}
GatewayIngressCounters GatewayCredentialIngress::counters() const noexcept {
    return impl_->counters();
}

}  // namespace realm::game::gateway
