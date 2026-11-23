#include "realmmesh/game/chat/chat_config.hpp"
#include <algorithm>
#include <set>
#include <stdexcept>
namespace realm::game::chat {
void validate_chat_config(const ChatConfig& c) {
    const auto identity = [](const std::string& text) {
        return !text.empty() && text.size() <= 128 &&
            std::ranges::all_of(text, [](unsigned char b) { return b >= 33 && b <= 126; });
    };
    const std::set<std::uint64_t> unique(c.channels.begin(), c.channels.end());
    if (c.realm_id == 0 || !identity(c.realm_instance_id) || !identity(c.realm_identity) ||
        c.channels.empty() || c.channels.size() > common::chat_channels_max ||
        unique.size() != c.channels.size() || *unique.rbegin() > 2 ||
        c.pending_requests == 0 || c.pending_requests > common::chat_pending_requests_max ||
        c.queued_per_link == 0 || c.queued_per_link > common::chat_queued_requests_per_link_max ||
        c.pending_pushes == 0 || c.pending_pushes > common::chat_pending_pushes_max ||
        c.pending_push_bytes == 0 || c.pending_push_bytes > common::chat_pending_push_bytes_max ||
        c.operation_timeout_ms == 0 || c.operation_timeout_ms > common::chat_operation_timeout_ms)
        throw std::invalid_argument("invalid bounded Chat configuration");
}
}  // namespace realm::game::chat
