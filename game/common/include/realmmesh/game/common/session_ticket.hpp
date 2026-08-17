#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace realm::game::common {

inline constexpr std::size_t session_ticket_key_size = 32;
inline constexpr std::size_t session_ticket_id_size = 16;
using SessionTicketKey = std::array<std::byte, session_ticket_key_size>;
using SessionTicketId = std::array<std::byte, session_ticket_id_size>;

enum class TicketPurpose : std::uint8_t {
    Login = 1,
    EnterGame = 2,
};

struct SessionTicketClaims {
    TicketPurpose purpose{TicketPurpose::Login};
    SessionTicketId ticket_id{};
    std::uint64_t account_id{0};
    std::uint32_t realm_id{0};
    std::uint64_t character_id{0};
    std::chrono::system_clock::time_point expires_at;
};

class SessionTicketCodec final {
public:
    explicit SessionTicketCodec(SessionTicketKey key);

    [[nodiscard]] std::vector<std::byte> issue(
        TicketPurpose purpose,
        std::uint64_t account_id,
        std::uint32_t realm_id,
        std::uint64_t character_id,
        std::chrono::seconds ttl,
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now()) const;

    [[nodiscard]] std::optional<SessionTicketClaims> validate(
        std::span<const std::byte> ticket,
        TicketPurpose expected_purpose,
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now()) const;

private:
    SessionTicketKey key_;
};

class TicketReplayGuard final {
public:
    [[nodiscard]] bool consume(
        const SessionTicketClaims& claims,
        std::chrono::system_clock::time_point now =
            std::chrono::system_clock::now());

private:
    struct TicketIdHash {
        std::size_t operator()(const SessionTicketId& id) const noexcept;
    };

    std::unordered_map<
        SessionTicketId,
        std::chrono::system_clock::time_point,
        TicketIdHash> consumed_;
};

[[nodiscard]] SessionTicketKey parse_ticket_key_hex(std::string_view value);

}  // namespace realm::game::common
