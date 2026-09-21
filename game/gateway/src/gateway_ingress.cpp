#include "realmmesh/game/gateway/gateway_ingress.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace realm::game::gateway {
namespace {

struct IpAddress final {
    int family{AF_UNSPEC};
    std::array<unsigned char, 16> bytes{};
};

struct Cidr final {
    IpAddress network;
    unsigned prefix{0};
};

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    const auto last = value.find_last_not_of(" \t");
    return std::string{value.substr(first, last - first + 1U)};
}

std::optional<IpAddress> parse_ip(std::string_view value) {
    const auto text = trim(value);
    if (text.empty()) return std::nullopt;
    IpAddress address;
    if (::inet_pton(AF_INET, text.c_str(), address.bytes.data()) == 1) {
        address.family = AF_INET;
        return address;
    }
    if (::inet_pton(AF_INET6, text.c_str(), address.bytes.data()) == 1) {
        address.family = AF_INET6;
        return address;
    }
    return std::nullopt;
}

std::optional<std::string> canonical_ip(std::string_view value) {
    const auto address = parse_ip(value);
    if (!address.has_value()) return std::nullopt;
    char output[INET6_ADDRSTRLEN]{};
    if (::inet_ntop(
            address->family,
            address->bytes.data(),
            output,
            sizeof(output)) == nullptr) {
        return std::nullopt;
    }
    return std::string{output};
}

std::optional<Cidr> parse_cidr(std::string_view value) {
    const auto slash = value.find('/');
    if (slash == std::string_view::npos ||
        value.find('/', slash + 1U) != std::string_view::npos) {
        return std::nullopt;
    }
    const auto address = parse_ip(value.substr(0, slash));
    if (!address.has_value()) return std::nullopt;
    const auto prefix_text = value.substr(slash + 1U);
    if (prefix_text.empty()) return std::nullopt;
    unsigned prefix = 0;
    for (const char character : prefix_text) {
        if (character < '0' || character > '9') return std::nullopt;
        const auto digit = static_cast<unsigned>(character - '0');
        if (prefix > (std::numeric_limits<unsigned>::max() - digit) / 10U) {
            return std::nullopt;
        }
        prefix = prefix * 10U + digit;
    }
    const auto maximum = address->family == AF_INET ? 32U : 128U;
    if (prefix > maximum) return std::nullopt;
    return Cidr{*address, prefix};
}

bool contains(const Cidr& cidr, const IpAddress& address) {
    if (cidr.network.family != address.family) return false;
    const auto whole_bytes = cidr.prefix / 8U;
    const auto remaining_bits = cidr.prefix % 8U;
    if (!std::equal(
            cidr.network.bytes.begin(),
            cidr.network.bytes.begin() + whole_bytes,
            address.bytes.begin())) {
        return false;
    }
    if (remaining_bits == 0U) return true;
    const auto mask = static_cast<unsigned char>(0xFFU << (8U - remaining_bits));
    return (cidr.network.bytes[whole_bytes] & mask) ==
        (address.bytes[whole_bytes] & mask);
}

std::vector<std::string> split(std::string_view value, char delimiter) {
    std::vector<std::string> result;
    std::size_t begin = 0;
    while (begin <= value.size()) {
        const auto end = value.find(delimiter, begin);
        result.push_back(trim(value.substr(
            begin,
            end == std::string_view::npos ? value.size() - begin
                                          : end - begin)));
        if (end == std::string_view::npos) break;
        begin = end + 1U;
    }
    return result;
}

std::optional<std::string> forwarded_for_value(std::string_view element) {
    for (const auto& parameter : split(element, ';')) {
        const auto equals = parameter.find('=');
        if (equals == std::string::npos) continue;
        auto name = trim(std::string_view{parameter}.substr(0, equals));
        std::ranges::transform(name, name.begin(), [](unsigned char value) {
            return static_cast<char>(std::tolower(value));
        });
        if (name != "for") continue;
        auto value = trim(std::string_view{parameter}.substr(equals + 1U));
        if (value.size() >= 2U && value.front() == '"' &&
            value.back() == '"') {
            value = value.substr(1U, value.size() - 2U);
        }
        if (value.empty() || value == "unknown" || value.front() == '_') {
            return std::nullopt;
        }
        if (value.front() == '[') {
            const auto close = value.find(']');
            if (close == std::string::npos) return std::nullopt;
            return canonical_ip(value.substr(1U, close - 1U));
        }
        if (const auto direct = canonical_ip(value); direct.has_value()) {
            return direct;
        }
        const auto colon = value.rfind(':');
        if (colon == std::string::npos) return std::nullopt;
        return canonical_ip(value.substr(0, colon));
    }
    return std::nullopt;
}

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

GatewaySourceMode parse_gateway_source_mode(std::string_view value) {
    if (value == "direct_peer") return GatewaySourceMode::DirectPeer;
    if (value == "trusted_x_forwarded_for") {
        return GatewaySourceMode::TrustedXForwardedFor;
    }
    if (value == "trusted_forwarded") {
        return GatewaySourceMode::TrustedForwarded;
    }
    throw std::invalid_argument("unsupported gateway source mode");
}

std::string_view to_string(GatewaySourceMode mode) noexcept {
    switch (mode) {
    case GatewaySourceMode::DirectPeer:
        return "direct_peer";
    case GatewaySourceMode::TrustedXForwardedFor:
        return "trusted_x_forwarded_for";
    case GatewaySourceMode::TrustedForwarded:
        return "trusted_forwarded";
    }
    return "unknown";
}

void GatewaySourceConfig::validate() const {
    if (trusted_proxy_cidrs.size() > gateway_trusted_proxy_hard_max) {
        throw std::invalid_argument("too many trusted proxy CIDRs");
    }
    if (mode == GatewaySourceMode::DirectPeer &&
        !trusted_proxy_cidrs.empty()) {
        throw std::invalid_argument(
            "direct peer mode cannot configure trusted proxy CIDRs");
    }
    if (mode != GatewaySourceMode::DirectPeer &&
        trusted_proxy_cidrs.empty()) {
        throw std::invalid_argument(
            "forwarded source mode requires trusted proxy CIDRs");
    }
    if (std::ranges::any_of(trusted_proxy_cidrs, [](const auto& cidr) {
            return !parse_cidr(cidr).has_value();
        })) {
        throw std::invalid_argument("invalid trusted proxy CIDR");
    }
}

class GatewaySourceNormalizer::Impl final {
public:
    explicit Impl(GatewaySourceConfig config) : config_(std::move(config)) {
        config_.validate();
        trusted_.reserve(config_.trusted_proxy_cidrs.size());
        for (const auto& text : config_.trusted_proxy_cidrs) {
            trusted_.push_back(*parse_cidr(text));
        }
    }

    std::optional<std::string> normalize(
        const network::TransportIngressSource& source) const {
        const auto direct = parse_ip(source.direct_peer);
        const auto canonical_direct = canonical_ip(source.direct_peer);
        if (!direct.has_value() || !canonical_direct.has_value()) {
            return std::nullopt;
        }
        if (config_.mode == GatewaySourceMode::DirectPeer ||
            !is_trusted(*direct)) {
            return canonical_direct;
        }

        std::vector<std::string> chain;
        if (config_.mode == GatewaySourceMode::TrustedXForwardedFor) {
            if (source.x_forwarded_for.empty()) return std::nullopt;
            for (const auto& entry : split(source.x_forwarded_for, ',')) {
                const auto parsed = canonical_ip(entry);
                if (!parsed.has_value()) return std::nullopt;
                chain.push_back(*parsed);
            }
        } else {
            if (source.forwarded.empty()) return std::nullopt;
            for (const auto& entry : split(source.forwarded, ',')) {
                const auto parsed = forwarded_for_value(entry);
                if (!parsed.has_value()) return std::nullopt;
                chain.push_back(*parsed);
            }
        }
        chain.push_back(*canonical_direct);
        for (auto iterator = chain.rbegin(); iterator != chain.rend();
             ++iterator) {
            const auto address = parse_ip(*iterator);
            if (!address.has_value()) return std::nullopt;
            if (!is_trusted(*address)) return *iterator;
        }
        return chain.front();
    }

private:
    bool is_trusted(const IpAddress& address) const {
        return std::ranges::any_of(trusted_, [&address](const auto& cidr) {
            return contains(cidr, address);
        });
    }

    GatewaySourceConfig config_;
    std::vector<Cidr> trusted_;
};

GatewaySourceNormalizer::GatewaySourceNormalizer(GatewaySourceConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {}
GatewaySourceNormalizer::~GatewaySourceNormalizer() = default;
GatewaySourceNormalizer::GatewaySourceNormalizer(
    GatewaySourceNormalizer&&) noexcept = default;
GatewaySourceNormalizer& GatewaySourceNormalizer::operator=(
    GatewaySourceNormalizer&&) noexcept = default;
std::optional<std::string> GatewaySourceNormalizer::normalize(
    const network::TransportIngressSource& source) const {
    return impl_->normalize(source);
}

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
