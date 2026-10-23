#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>

namespace realm::game::common {
inline constexpr std::size_t chat_envelope_max = 64 * 1024;
inline constexpr std::size_t chat_text_bytes_max = 512;
inline constexpr std::uint64_t chat_counter_max =
    static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
inline constexpr std::size_t chat_channels_max = 3;
inline constexpr std::size_t chat_instances_max = 2;
inline constexpr std::size_t chat_characters_max = 64;
inline constexpr std::size_t chat_memberships_per_character_max = 2;
inline constexpr std::size_t chat_scene_members_max = 16;
inline constexpr std::size_t chat_realm_members_max = 64;
inline constexpr std::size_t chat_sends_in_flight_per_membership_max = 1;
inline constexpr std::uint32_t chat_send_rate_per_second = 5;
inline constexpr std::uint32_t chat_send_burst = 10;
inline constexpr std::size_t chat_pending_requests_max = 64;
inline constexpr std::size_t chat_queued_requests_per_link_max = 8;
inline constexpr std::uint32_t chat_operation_timeout_ms = 1000;
inline constexpr std::uint32_t chat_retry_after_ms_max = 1000;
inline constexpr std::size_t chat_pending_pushes_max = 1024;
inline constexpr std::size_t chat_pending_push_bytes_max = 4 * 1024 * 1024;
inline constexpr std::size_t chat_client_notifications_max = 256;
inline constexpr std::size_t chat_client_notification_bytes_max = 256 * 1024;
inline constexpr std::size_t chat_transport_connections_max = 16;
inline constexpr std::size_t chat_requests_per_round_max = 64;
inline constexpr std::size_t chat_pushes_per_round_max = 64;
inline constexpr std::size_t chat_new_sends_per_membership_per_round_max = 1;
inline constexpr std::size_t chat_link_outbound_bytes_max = 1024 * 1024;
inline constexpr std::size_t chat_message_recipients_max = 64;

// 计数和字节必须同时可预留。used 包括运行、排队及未收结果；运行时负责原子记账。
struct ChatBudget {
    std::size_t count{};
    std::size_t bytes{};
};
[[nodiscard]] constexpr bool chat_budget_allows(
    ChatBudget used, ChatBudget incoming, ChatBudget limit) noexcept {
    return used.count <= limit.count && used.bytes <= limit.bytes &&
           incoming.count <= limit.count - used.count &&
           incoming.bytes <= limit.bytes - used.bytes;
}
}  // namespace realm::game::common
