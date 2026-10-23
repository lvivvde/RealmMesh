#pragma once
#include "realmmesh/game/chat/chat_channels.hpp"
#include <stdexcept>
namespace realm::game::chat::test {
inline wire::Membership member(std::uint64_t generation, std::uint64_t character = 7, std::uint64_t shard = 1) {
    wire::Membership m;
    m.mutable_channel()->set_realm_id(1);
    m.mutable_channel()->set_kind(shard ? wire::CHANNEL_KIND_SCENE : wire::CHANNEL_KIND_REALM);
    m.mutable_channel()->set_shard_id(shard); m.set_character_id(character);
    m.set_realm_incarnation("realm-run-1"); m.set_session_binding(11);
    m.set_generation(generation); if (shard) m.set_scene_binding_epoch(1);
    return m;
}
inline common::ChatInternalRequest hello() {
    common::ChatInternalRequest r;
    r.set_operation(wire::OPERATION_HELLO);
    auto* l = r.mutable_link(); l->set_realm_id(1); l->set_realm_instance_id("realm-dev-01");
    l->set_realm_incarnation("realm-run-1"); l->set_chat_instance_id("chat-dev-01");
    return r;
}
inline common::ChatInternalRequest request(const wire::Link& link, wire::Operation op, const wire::Membership& m) {
    common::ChatInternalRequest r; *r.mutable_link() = link; r.set_operation(op); *r.mutable_membership() = m;
    return r;
}
inline common::ChatInternalRequest send(const wire::Link& link, const wire::Membership& m,
                                        std::uint64_t seq = 1, std::string text = "你好") {
    auto r = request(link, wire::OPERATION_SEND, m);
    auto* s = r.mutable_send(); s->set_kind(m.channel().kind()); s->set_membership_generation(m.generation());
    *s->mutable_expected_epoch()->mutable_channel() = m.channel();
    s->mutable_expected_epoch()->set_chat_incarnation(link.chat_incarnation());
    s->set_client_send_seq(seq); s->set_text(std::move(text)); return r;
}
class World {
public:
    explicit World(ChatConfig c = {}, std::string incarnation = "chat-run-1")
        : channels(std::move(c), "chat-dev-01", std::move(incarnation)) {
        if (!channels.opened(1)) throw std::runtime_error("cannot open test peer");
        link = call(hello()).link();
    }
    common::ChatInternalReply call(const common::ChatInternalRequest& r, ChatChannels::Peer peer = 1) {
        if (!channels.submit(peer, r, ++id, now)) throw std::runtime_error("test queue rejected");
        channels.run_round(now);
        const auto result = channels.take_reply();
        if (!result) throw std::runtime_error("missing test reply");
        // 每个状态机回复都必须符合生产线契约。
        if (!common::decode_chat_internal_reply(common::encode(result->message, result->request_id)))
            throw std::runtime_error("invalid reply contract");
        return result->message;
    }
    common::ChatInternalReply join(const wire::Membership& m) { return call(request(link, wire::OPERATION_JOIN, m)); }
    std::vector<common::ChatInternalPush> drain() {
        std::vector<common::ChatInternalPush> result;
        while (channels.pending_pushes() != 0) {
            for (const auto& p : channels.drain_pushes()) {
                const auto decoded = common::decode_chat_internal_push(p.payload);
                if (!decoded) throw std::runtime_error("invalid push contract");
                result.push_back(*decoded);
            }
        }
        return result;
    }
    ChatChannels channels;
    wire::Link link;
    ChatChannels::Clock::time_point now{};
    std::uint64_t id{};
};
}  // namespace realm::game::chat::test
