#include "realmmesh/game/common/session_ticket.hpp"

#include <sodium.h>

#include <algorithm>
#include <limits>
#include <stdexcept>
#include <string>

namespace realm::game::common {
namespace {

constexpr std::byte ticket_version_v1{1};
constexpr std::byte ticket_version_v2{2};
constexpr std::size_t signed_size_v1 =
    1 + 1 + session_ticket_id_size + 8 + 4 + 8 + 8;
constexpr std::size_t signed_size_v2 =
    1 + 1 + session_ticket_id_size + correlation_id_size + 8 + 4 + 8 + 8;
constexpr std::size_t tag_size = crypto_auth_hmacsha256_BYTES;
constexpr std::size_t ticket_size_v1 = signed_size_v1 + tag_size;
constexpr std::size_t ticket_size_v2 = signed_size_v2 + tag_size;

void write_unsigned(std::span<std::byte> output, std::uint64_t value) {
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto shift = static_cast<unsigned>((output.size() - index - 1) * 8);
        output[index] = static_cast<std::byte>((value >> shift) & 0xffU);
    }
}

std::uint64_t read_unsigned(std::span<const std::byte> input) {
    std::uint64_t value = 0;
    for (const auto byte : input) {
        value = (value << 8U) | std::to_integer<std::uint8_t>(byte);
    }
    return value;
}

int hex_nibble(char value) {
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

std::vector<std::byte> issue_ticket(
    const SessionTicketKey& key,
    TicketPurpose purpose,
    std::uint64_t account_id,
    std::uint32_t realm_id,
    std::uint64_t character_id,
    const std::optional<CorrelationId>& correlation_id,
    std::chrono::seconds ttl,
    std::chrono::system_clock::time_point now) {
    if (ttl <= std::chrono::seconds::zero() || account_id == 0) {
        throw std::invalid_argument("ticket TTL and account id must be positive");
    }
    const auto expiry = std::chrono::duration_cast<std::chrono::milliseconds>(
        (now + ttl).time_since_epoch()).count();
    if (expiry < 0) {
        throw std::invalid_argument("ticket expiry cannot precede Unix epoch");
    }

    const bool v2 = correlation_id.has_value();
    const auto signed_size = v2 ? signed_size_v2 : signed_size_v1;
    std::vector<std::byte> ticket(v2 ? ticket_size_v2 : ticket_size_v1);
    ticket[0] = v2 ? ticket_version_v2 : ticket_version_v1;
    ticket[1] = static_cast<std::byte>(purpose);
    randombytes_buf(ticket.data() + 2, session_ticket_id_size);
    std::size_t offset = 2 + session_ticket_id_size;
    if (correlation_id.has_value()) {
        std::ranges::copy(
            *correlation_id,
            ticket.begin() + static_cast<std::ptrdiff_t>(offset));
        offset += correlation_id_size;
    }
    write_unsigned(std::span<std::byte>(ticket.data() + offset, 8), account_id);
    offset += 8;
    write_unsigned(std::span<std::byte>(ticket.data() + offset, 4), realm_id);
    offset += 4;
    write_unsigned(std::span<std::byte>(ticket.data() + offset, 8), character_id);
    offset += 8;
    write_unsigned(
        std::span<std::byte>(ticket.data() + offset, 8),
        static_cast<std::uint64_t>(expiry));

    crypto_auth_hmacsha256(
        reinterpret_cast<unsigned char*>(ticket.data() + signed_size),
        reinterpret_cast<const unsigned char*>(ticket.data()),
        signed_size,
        reinterpret_cast<const unsigned char*>(key.data()));
    return ticket;
}

}  // namespace

SessionTicketCodec::SessionTicketCodec(SessionTicketKey key) : key_(key) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    if (std::ranges::all_of(key_, [](std::byte value) {
            return value == std::byte{0};
        })) {
        throw std::invalid_argument("session ticket key cannot be all zero");
    }
}

std::vector<std::byte> SessionTicketCodec::issue(
    TicketPurpose purpose,
    std::uint64_t account_id,
    std::uint32_t realm_id,
    std::uint64_t character_id,
    std::chrono::seconds ttl,
    std::chrono::system_clock::time_point now) const {
    return issue_ticket(
        key_,
        purpose,
        account_id,
        realm_id,
        character_id,
        std::nullopt,
        ttl,
        now);
}

std::vector<std::byte> SessionTicketCodec::issue(
    TicketPurpose purpose,
    std::uint64_t account_id,
    std::uint32_t realm_id,
    std::uint64_t character_id,
    const CorrelationId& correlation_id,
    std::chrono::seconds ttl,
    std::chrono::system_clock::time_point now) const {
    return issue_ticket(
        key_,
        purpose,
        account_id,
        realm_id,
        character_id,
        correlation_id,
        ttl,
        now);
}

std::optional<SessionTicketClaims> SessionTicketCodec::validate(
    std::span<const std::byte> ticket,
    TicketPurpose expected_purpose,
    std::chrono::system_clock::time_point now) const {
    if (ticket.empty()) return std::nullopt;
    const bool v1 = ticket[0] == ticket_version_v1;
    const bool v2 = ticket[0] == ticket_version_v2;
    const auto signed_size = v2 ? signed_size_v2 : signed_size_v1;
    const auto ticket_size = v2 ? ticket_size_v2 : ticket_size_v1;
    if ((!v1 && !v2) || ticket.size() != ticket_size ||
        ticket[1] != static_cast<std::byte>(expected_purpose)) {
        return std::nullopt;
    }
    if (crypto_auth_hmacsha256_verify(
            reinterpret_cast<const unsigned char*>(ticket.data() + signed_size),
            reinterpret_cast<const unsigned char*>(ticket.data()),
            signed_size,
            reinterpret_cast<const unsigned char*>(key_.data())) != 0) {
        return std::nullopt;
    }

    SessionTicketClaims claims;
    claims.purpose = expected_purpose;
    std::ranges::copy(
        ticket.subspan(2, session_ticket_id_size), claims.ticket_id.begin());
    std::size_t offset = 2 + session_ticket_id_size;
    if (v2) {
        CorrelationId correlation_id{};
        std::ranges::copy(
            ticket.subspan(offset, correlation_id_size),
            correlation_id.begin());
        claims.correlation_id = correlation_id;
        offset += correlation_id_size;
    }
    claims.account_id = read_unsigned(ticket.subspan(offset, 8));
    offset += 8;
    claims.realm_id = static_cast<std::uint32_t>(
        read_unsigned(ticket.subspan(offset, 4)));
    offset += 4;
    claims.character_id = read_unsigned(ticket.subspan(offset, 8));
    offset += 8;
    const auto expiry_ms = read_unsigned(ticket.subspan(offset, 8));
    if (expiry_ms > static_cast<std::uint64_t>(
                        std::numeric_limits<std::int64_t>::max())) {
        return std::nullopt;
    }
    claims.expires_at = std::chrono::system_clock::time_point(
        std::chrono::milliseconds(static_cast<std::int64_t>(expiry_ms)));
    if (claims.account_id == 0 || claims.expires_at <= now) {
        return std::nullopt;
    }
    return claims;
}

std::size_t TicketReplayGuard::TicketIdHash::operator()(
    const SessionTicketId& id) const noexcept {
    std::size_t hash = 0;
    for (const auto value : id) {
        hash = hash * 131U + std::to_integer<std::uint8_t>(value);
    }
    return hash;
}

bool TicketReplayGuard::consume(
    const SessionTicketClaims& claims,
    std::chrono::system_clock::time_point now) {
    std::erase_if(consumed_, [now](const auto& entry) {
        return entry.second <= now;
    });
    return consumed_.emplace(claims.ticket_id, claims.expires_at).second;
}

SessionTicketKey parse_ticket_key_hex(std::string_view value) {
    if (value.size() != session_ticket_key_size * 2) {
        throw std::invalid_argument("session ticket key must contain 64 hex characters");
    }
    SessionTicketKey key{};
    for (std::size_t index = 0; index < key.size(); ++index) {
        const int high = hex_nibble(value[index * 2]);
        const int low = hex_nibble(value[index * 2 + 1]);
        if (high < 0 || low < 0) {
            throw std::invalid_argument("session ticket key contains non-hex data");
        }
        key[index] = static_cast<std::byte>((high << 4) | low);
    }
    return key;
}

CorrelationId make_correlation_id() {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    CorrelationId value{};
    randombytes_buf(value.data(), value.size());
    return value;
}

std::string correlation_id_hex(const CorrelationId& value) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(value.size() * 2);
    for (const auto byte : value) {
        const auto numeric = std::to_integer<std::uint8_t>(byte);
        result.push_back(digits[(numeric >> 4U) & 0x0fU]);
        result.push_back(digits[numeric & 0x0fU]);
    }
    return result;
}

}  // namespace realm::game::common
