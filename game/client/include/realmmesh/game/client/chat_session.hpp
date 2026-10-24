#pragma once
#include <optional>
#include <variant>
#include "realmmesh/game/client/login_chain.hpp"
#include "realmmesh/game/common/chat_protocol.hpp"
namespace realm::game::client {
using ChatNotice = std::variant<common::ChatMessage, common::ChatUnavailable>;
/// 与 Scene/训练/心跳共享 Realm Session，一次一个调用。应用显式构造发送，
/// Unknown 只允许原资格、原号、原参数查询/重试；本接口从不自动重发。
class ChatSession {
public:
    virtual ~ChatSession() = default;
    virtual RealmReply<common::ChatState> get_chat_state(
        ::realmmesh::protocol::chat::v1::ChannelKind, TimePoint deadline) = 0;
    virtual RealmReply<common::ChatSendResult> send_chat(
        const common::SendChat&, TimePoint deadline) = 0;
    virtual std::optional<ChatNotice> poll_chat(TimePoint deadline) = 0;
};
}  // namespace realm::game::client
