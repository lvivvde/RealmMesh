#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
#include "realmmesh/game/common/chat_limits.hpp"
namespace realm::service_host { class LayeredConfig; }
namespace realm::game::chat {
struct ChatConfig {
    std::uint32_t realm_id{1};
    std::string realm_instance_id{"realm-dev-01"};
    std::string realm_identity{"realm-dev-01.realm.service"};
    // 0 = Realm Channel，1/2 = Scene Channel；只列本实例唯一承载的频道。
    std::vector<std::uint64_t> channels{0, 1, 2};
    std::size_t pending_requests{common::chat_pending_requests_max};
    std::size_t queued_per_link{common::chat_queued_requests_per_link_max};
    std::size_t pending_pushes{common::chat_pending_pushes_max};
    std::size_t pending_push_bytes{common::chat_pending_push_bytes_max};
    std::uint32_t operation_timeout_ms{common::chat_operation_timeout_ms};
};
void validate_chat_config(const ChatConfig& config);
[[nodiscard]] ChatConfig load_chat_config(const service_host::LayeredConfig& config);
}  // namespace realm::game::chat
