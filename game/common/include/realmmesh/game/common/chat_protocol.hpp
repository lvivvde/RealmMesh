#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>
#include "realmmesh/chat/v1/chat.pb.h"
#include "realmmesh/game/common/chat_limits.hpp"

namespace realm::game::common {
inline constexpr const char* chat_internal_alpn = "realmmesh-chat/1";
inline constexpr int chat_error_invalid_request = 3201;
inline constexpr int chat_error_character_not_selected = 3202;
using ChatMessageId = ::realmmesh::protocol::chat::v1::MessageId;
using GetChatState = ::realmmesh::protocol::chat::v1::GetChatState;
using ChatState = ::realmmesh::protocol::chat::v1::ChatState;
using SendChat = ::realmmesh::protocol::chat::v1::SendChat;
using ChatSendResult = ::realmmesh::protocol::chat::v1::ChatSendResult;
using ChatMessage = ::realmmesh::protocol::chat::v1::ChatMessage;
using ChatUnavailable = ::realmmesh::protocol::chat::v1::ChatUnavailable;
using ChatInternalRequest = ::realmmesh::protocol::chat::v1::ChatInternalRequest;
using ChatInternalReply = ::realmmesh::protocol::chat::v1::ChatInternalReply;
using ChatInternalPush = ::realmmesh::protocol::chat::v1::ChatInternalPush;
// 结构合法不代表已认证/有权限；接收者还须核对活动链路、当前会话与配置承载。
[[nodiscard]] bool valid_chat_text(std::string_view text) noexcept;
[[nodiscard]] std::optional<ChatMessageId> chat_message_id(std::span<const std::byte> payload);
[[nodiscard]] std::optional<ChatMessageId> chat_internal_message_id(std::span<const std::byte> payload);
[[nodiscard]] std::optional<std::uint64_t> chat_request_id(std::span<const std::byte> payload);
[[nodiscard]] std::optional<std::uint64_t> chat_internal_request_id(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const GetChatState& message, std::uint64_t request_id);
[[nodiscard]] std::optional<GetChatState> decode_get_chat_state(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const ChatState& message, std::uint64_t request_id);
[[nodiscard]] std::optional<ChatState> decode_chat_state(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const SendChat& message, std::uint64_t request_id);
[[nodiscard]] std::optional<SendChat> decode_send_chat(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const ChatSendResult& message, std::uint64_t request_id);
[[nodiscard]] std::optional<ChatSendResult> decode_chat_send_result(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const ChatMessage& message, std::uint64_t request_id = 0);
[[nodiscard]] std::optional<ChatMessage> decode_chat_message(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const ChatUnavailable& message, std::uint64_t request_id = 0);
[[nodiscard]] std::optional<ChatUnavailable> decode_chat_unavailable(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const ChatInternalRequest& message, std::uint64_t request_id);
[[nodiscard]] std::optional<ChatInternalRequest> decode_chat_internal_request(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const ChatInternalReply& message, std::uint64_t request_id);
[[nodiscard]] std::optional<ChatInternalReply> decode_chat_internal_reply(std::span<const std::byte> payload);
[[nodiscard]] std::vector<std::byte> encode(const ChatInternalPush& message, std::uint64_t request_id = 0);
[[nodiscard]] std::optional<ChatInternalPush> decode_chat_internal_push(std::span<const std::byte> payload);
}  // namespace realm::game::common
