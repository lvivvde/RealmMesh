#include "realmmesh/game/chat/chat_channels.hpp"
#include <algorithm>
#include <limits>
#include <set>
#include <stdexcept>

namespace realm::game::chat {
namespace {
bool same_channel(const wire::Channel& a, const wire::Channel& b) {
    return a.realm_id() == b.realm_id() && a.kind() == b.kind() && a.shard_id() == b.shard_id();
}
bool same_member(const wire::Membership& a, const wire::Membership& b) {
    return same_channel(a.channel(), b.channel()) && a.character_id() == b.character_id() &&
        a.realm_incarnation() == b.realm_incarnation() && a.session_binding() == b.session_binding() &&
        a.generation() == b.generation() && a.scene_binding_epoch() == b.scene_binding_epoch();
}
bool same_link(const wire::Link& a, const wire::Link& b) {
    return a.realm_id() == b.realm_id() && a.realm_instance_id() == b.realm_instance_id() &&
        a.realm_incarnation() == b.realm_incarnation() && a.chat_instance_id() == b.chat_instance_id() &&
        a.chat_incarnation() == b.chat_incarnation() && a.link_generation() == b.link_generation();
}
// 未知字段可在线解码，但不能原样放大到资格、回执或扇出中。
wire::Channel known_channel(const wire::Channel& source) {
    wire::Channel value;
    value.set_realm_id(source.realm_id()); value.set_kind(source.kind()); value.set_shard_id(source.shard_id());
    return value;
}
common::ChatInternalRequest known_request(const common::ChatInternalRequest& source) {
    common::ChatInternalRequest value;
    value.set_operation(source.operation());
    const auto& l = source.link(); auto* link = value.mutable_link();
    link->set_realm_id(l.realm_id()); link->set_realm_instance_id(l.realm_instance_id());
    link->set_realm_incarnation(l.realm_incarnation()); link->set_chat_instance_id(l.chat_instance_id());
    link->set_chat_incarnation(l.chat_incarnation()); link->set_link_generation(l.link_generation());
    if (source.has_membership()) {
        const auto& m = source.membership(); auto* member = value.mutable_membership();
        *member->mutable_channel() = known_channel(m.channel()); member->set_character_id(m.character_id());
        member->set_realm_incarnation(m.realm_incarnation()); member->set_session_binding(m.session_binding());
        member->set_generation(m.generation()); member->set_scene_binding_epoch(m.scene_binding_epoch());
    }
    if (source.has_send()) {
        const auto& s = source.send(); auto* send = value.mutable_send();
        send->set_kind(s.kind()); send->set_membership_generation(s.membership_generation());
        send->set_client_send_seq(s.client_send_seq()); send->set_text(s.text());
        *send->mutable_expected_epoch()->mutable_channel() = known_channel(s.expected_epoch().channel());
        send->mutable_expected_epoch()->set_chat_incarnation(s.expected_epoch().chat_incarnation());
    }
    return value;
}
}  // namespace
common::ChatInternalReply chat_reply(const common::ChatInternalRequest& r, wire::Status status) {
    common::ChatInternalReply reply;
    *reply.mutable_link() = known_request(r).link();
    reply.set_operation(r.operation());
    reply.set_status(status);
    return reply;
}

ChatChannels::ChatChannels(ChatConfig config, std::string instance_id, std::string incarnation)
    : config_(std::move(config)), instance_id_(std::move(instance_id)), incarnation_(std::move(incarnation)) {
    validate_chat_config(config_);
    for (const auto shard : config_.channels) {
        Channel channel;
        auto* identity = channel.epoch.mutable_channel();
        identity->set_realm_id(config_.realm_id);
        identity->set_kind(shard == 0 ? wire::CHANNEL_KIND_REALM : wire::CHANNEL_KIND_SCENE);
        identity->set_shard_id(shard);
        channel.epoch.set_chat_incarnation(incarnation_);
        channels_.emplace(shard, std::move(channel));
    }
    // 借用权威线校验，避免构造出无法编码的运行身份。
    common::ChatInternalReply probe;
    auto* link = probe.mutable_link();
    link->set_realm_id(config_.realm_id);
    link->set_realm_instance_id(config_.realm_instance_id);
    link->set_realm_incarnation("validation");
    link->set_chat_instance_id(instance_id_);
    link->set_chat_incarnation(incarnation_);
    link->set_link_generation(1);
    probe.set_operation(wire::OPERATION_HELLO);
    probe.set_status(wire::STATUS_OK);
    static_cast<void>(common::encode(probe, 1));
}
std::optional<std::uint64_t> ChatChannels::next_counter(std::uint64_t value) noexcept {
    if (value >= common::chat_counter_max) return std::nullopt;
    return value + 1;
}
bool ChatChannels::opened(Peer peer) {
    if (peer == 0 || links_.contains(peer) || links_.size() + closed_peers_.size() >= common::chat_transport_connections_max)
        return false;
    links_.emplace(peer, LinkState{});
    return true;
}
void ChatChannels::closed(Peer peer) {
    if (const auto found = links_.find(peer); found != links_.end()) {
        pending_ -= found->second.requests.size();
        links_.erase(found);
    }
    for (auto it = replies_.begin(); it != replies_.end();) {
        if (it->peer == peer) { it = replies_.erase(it); --pending_; }
        else ++it;
    }
    for (auto it = pushes_.begin(); it != pushes_.end();) {
        if (it->peer == peer) { push_bytes_ -= it->payload.size(); it = pushes_.erase(it); }
        else ++it;
    }
    for (auto& [shard, channel] : channels_) {
        static_cast<void>(shard);
        std::erase_if(channel.members, [peer](const auto& entry) { return entry.second.peer == peer; });
    }
}
bool ChatChannels::submit(Peer peer, const common::ChatInternalRequest& request,
                           std::uint64_t request_id, Clock::time_point now) {
    const auto found = links_.find(peer);
    if (found == links_.end()) return false;
    const auto same_send = [&](const auto& queued) {
        return queued.operation() == wire::OPERATION_SEND && request.operation() == wire::OPERATION_SEND &&
               queued.membership().generation() == request.membership().generation();
    };
    const bool send_in_flight = std::ranges::any_of(found->second.requests, [&](const Pending& p) { return same_send(p.request); }) ||
        std::ranges::any_of(replies_, [&](const Reply& p) {
            return p.peer == peer && p.message.operation() == wire::OPERATION_SEND &&
                request.operation() == wire::OPERATION_SEND && p.message.has_membership() &&
                p.message.membership().generation() == request.membership().generation();
        });
    if (send_in_flight || pending_ >= config_.pending_requests || found->second.requests.size() >= config_.queued_per_link) {
        ++stats_.queue_rejected;
        return false;
    }
    // 入队也校验，独立使用状态机的消费者不能绕过格式与64KiB预算。
    static_cast<void>(common::encode(request, request_id));
    found->second.requests.push_back({known_request(request), request_id, now + std::chrono::milliseconds(config_.operation_timeout_ms)});
    ++pending_;
    return true;
}
void ChatChannels::run_round(Clock::time_point now) {
    std::set<std::pair<Peer, std::uint64_t>> served;
    std::size_t processed = 0;
    for (std::size_t attempt = 0; attempt < common::chat_requests_per_round_max * common::chat_transport_connections_max &&
         processed < common::chat_requests_per_round_max && !links_.empty(); ++attempt) {
        auto link = links_.lower_bound(next_peer_);
        if (link == links_.end()) link = links_.begin();
        const auto peer = link->first;
        next_peer_ = peer == std::numeric_limits<Peer>::max() ? 0 : peer + 1;
        auto& queue = link->second.requests;
        const auto item = std::find_if(queue.begin(), queue.end(), [&](const Pending& p) {
            return p.deadline <= now || p.request.operation() != wire::OPERATION_SEND ||
                !served.contains({peer, p.request.membership().generation()});
        });
        if (item == queue.end()) continue;
        auto pending = std::move(*item);
        queue.erase(item);
        common::ChatInternalReply reply;
        if (pending.deadline <= now) {
            ++stats_.expired;
            reply = chat_reply(pending.request, wire::STATUS_UNAVAILABLE);
        } else {
            if (pending.request.operation() == wire::OPERATION_SEND)
                served.emplace(peer, pending.request.membership().generation());
            reply = handle(peer, pending, now);
        }
        replies_.push_back({peer, pending.id, std::move(reply),
            std::chrono::duration<double>(now - pending.deadline + std::chrono::milliseconds(config_.operation_timeout_ms)).count()});
        ++processed;
    }
}
std::optional<ChatChannels::Reply> ChatChannels::take_reply() {
    if (replies_.empty()) return std::nullopt;
    auto reply = std::move(replies_.front()); replies_.pop_front(); --pending_;
    return reply;
}
std::vector<ChatChannels::Push> ChatChannels::drain_pushes() {
    std::vector<Push> ready;
    for (std::size_t consumed = 0; consumed < common::chat_pushes_per_round_max && !pushes_.empty(); ++consumed) {
        auto push = std::move(pushes_.front()); pushes_.pop_front(); push_bytes_ -= push.payload.size();
        const auto channel = channels_.find(push.membership.channel().shard_id());
        if (channel == channels_.end()) continue;
        const auto member = channel->second.members.find(push.membership.character_id());
        if (member != channel->second.members.end() && member->second.peer == push.peer &&
            same_member(member->second.membership, push.membership))
            ready.push_back(Push{push.peer, std::move(push.payload)});
    }
    return ready;
}
std::vector<ChatChannels::Peer> ChatChannels::take_closed_peers() {
    auto peers = std::move(closed_peers_); closed_peers_.clear(); return peers;
}
std::size_t ChatChannels::active_members() const noexcept {
    std::size_t count = 0;
    for (const auto& [shard, channel] : channels_) { static_cast<void>(shard); count += channel.members.size(); }
    return count;
}
common::ChatInternalReply ChatChannels::handle(Peer peer, const Pending& pending, Clock::time_point now) {
    const auto& r = pending.request;
    auto reply = chat_reply(r, wire::STATUS_OK);
    auto& link = links_.at(peer);
    if (r.link().realm_id() != config_.realm_id || r.link().realm_instance_id() != config_.realm_instance_id ||
        r.link().chat_instance_id() != instance_id_) return chat_reply(r, wire::STATUS_FORBIDDEN);
    if (r.operation() == wire::OPERATION_HELLO) {
        const auto next = next_counter(link_generation_);
        if (link.link || !next) return chat_reply(r, wire::STATUS_INVALID);
        std::vector<Peer> replaced;
        for (const auto& [id, candidate] : links_) if (id != peer && candidate.link) replaced.push_back(id);
        for (const auto id : replaced) { closed(id); closed_peers_.push_back(id); }
        auto fresh = r.link(); fresh.set_link_generation(*next); fresh.set_chat_incarnation(incarnation_);
        link_generation_ = *next; link.link = fresh;
        *reply.mutable_link() = fresh;
        return reply;
    }
    if (!link.link || !same_link(*link.link, r.link())) {
        ++stats_.stale_rejected;
        return chat_reply(r, wire::STATUS_STALE_LINK);
    }
    const auto channel_it = channels_.find(r.membership().channel().shard_id());
    if (channel_it == channels_.end() || !same_channel(channel_it->second.epoch.channel(), r.membership().channel()))
        return chat_reply(r, wire::STATUS_FORBIDDEN);
    auto& channel = channel_it->second;
    auto member = channel.members.find(r.membership().character_id());
    const bool same = member != channel.members.end() && member->second.peer == peer &&
                      same_member(member->second.membership, r.membership());
    if (r.operation() == wire::OPERATION_LEAVE) {
        link.highest_generation = std::max(link.highest_generation, r.membership().generation());
        if (same) channel.members.erase(member);
        *reply.mutable_membership() = r.membership();
        return reply;
    }
    if (r.operation() == wire::OPERATION_JOIN) {
        if (same) {
            *reply.mutable_membership() = r.membership(); *reply.mutable_state() = member->second.state;
            return reply;
        }
        if (r.membership().generation() <= link.highest_generation) {
            ++stats_.stale_rejected; return chat_reply(r, wire::STATUS_STALE_MEMBERSHIP);
        }
        const auto capacity = channel.epoch.channel().kind() == wire::CHANNEL_KIND_SCENE ?
            common::chat_scene_members_max : common::chat_realm_members_max;
        std::set<std::uint64_t> characters;
        std::size_t qualifications = 0;
        for (const auto& [shard, c] : channels_) {
            static_cast<void>(shard);
            for (const auto& [id, m] : c.members) {
                static_cast<void>(m); characters.insert(id);
                if (id == r.membership().character_id()) ++qualifications;
            }
        }
        if ((member == channel.members.end() && channel.members.size() >= capacity) ||
            (!characters.contains(r.membership().character_id()) && characters.size() >= common::chat_characters_max) ||
            (member == channel.members.end() && qualifications >= common::chat_memberships_per_character_max)) {
            ++stats_.resource_rejected; return chat_reply(r, wire::STATUS_BUSY);
        }
        Member fresh{.peer = peer, .membership = r.membership(), .state = {}, .last_text = {},
                     .tokens = static_cast<double>(common::chat_send_burst), .refill = now};
        fresh.state.set_kind(channel.epoch.channel().kind()); fresh.state.set_phase(wire::PHASE_ACTIVE);
        fresh.state.set_membership_generation(r.membership().generation());
        *fresh.state.mutable_epoch() = channel.epoch; fresh.state.set_join_watermark(channel.sequence);
        link.highest_generation = r.membership().generation();
        member = channel.members.insert_or_assign(r.membership().character_id(), std::move(fresh)).first;
    } else if (!same) {
        ++stats_.stale_rejected; return chat_reply(r, wire::STATUS_STALE_MEMBERSHIP);
    }
    *reply.mutable_membership() = r.membership();
    if (r.operation() == wire::OPERATION_SEND) {
        auto* result = reply.mutable_result();
        result->set_kind(r.send().kind()); result->set_membership_generation(r.send().membership_generation());
        *result->mutable_epoch() = r.send().expected_epoch(); result->set_client_send_seq(r.send().client_send_seq());
        send(channel, member->second, r.send(), *result, now);
    } else *reply.mutable_state() = member->second.state;
    return reply;
}
void ChatChannels::send(Channel& channel, Member& member, const common::SendChat& request,
                         common::ChatSendResult& result, Clock::time_point now) {
    const auto reject = [&](wire::Reason reason) {
        result.set_outcome(wire::OUTCOME_REJECTED); result.set_reason(reason); ++stats_.rejected;
        if (reason == wire::REASON_RATE_LIMITED || reason == wire::REASON_RESOURCE_LIMIT)
            result.set_retry_after_ms(common::chat_retry_after_ms_max);
    };
    if (request.expected_epoch().chat_incarnation() != incarnation_) { reject(wire::REASON_WRONG_EPOCH); return; }
    const auto last = member.state.last_client_send_seq();
    if (request.client_send_seq() == last && last != 0) {
        if (member.last_text != request.text()) { reject(wire::REASON_SEQUENCE_CONFLICT); return; }
        result.set_outcome(wire::OUTCOME_ACCEPTED); *result.mutable_receipt() = member.state.last_receipt();
        ++stats_.replayed; return;
    }
    const auto next_send = next_counter(last), next_channel = next_counter(channel.sequence);
    if (!next_send || !next_channel) { reject(wire::REASON_COUNTER_EXHAUSTED); return; }
    if (request.client_send_seq() != *next_send) { reject(wire::REASON_INVALID_SEQUENCE); return; }
    if (now > member.refill) {
        member.tokens = std::min(static_cast<double>(common::chat_send_burst), member.tokens +
            std::chrono::duration<double>(now - member.refill).count() * common::chat_send_rate_per_second);
        member.refill = now;
    }
    if (member.tokens < 1) { ++stats_.rate_rejected; reject(wire::REASON_RATE_LIMITED); return; }
    std::deque<PendingPush> reserved;
    std::size_t bytes = 0;
    for (const auto& [id, recipient] : channel.members) {
        static_cast<void>(id);
        common::ChatInternalPush push;
        *push.mutable_link() = *links_.at(recipient.peer).link;
        *push.mutable_membership() = recipient.membership;
        auto* message = push.mutable_message();
        message->set_membership_generation(recipient.membership.generation());
        *message->mutable_epoch() = channel.epoch; message->set_channel_seq(*next_channel);
        message->set_sender_character_id(member.membership.character_id()); message->set_text(request.text());
        auto payload = common::encode(push);
        bytes += payload.size();
        reserved.push_back({recipient.peer, recipient.membership, std::move(payload)});
    }
    if (!common::chat_budget_allows({pushes_.size(), push_bytes_}, {reserved.size(), bytes},
            {config_.pending_pushes, config_.pending_push_bytes})) {
        ++stats_.resource_rejected; reject(wire::REASON_RESOURCE_LIMIT); return;
    }
    // 所有可能失败的分配在受理前完成。队列以swap提交，回执/文本使用protobuf Swap与string swap。
    auto combined = pushes_;
    for (auto& push : reserved) combined.push_back(std::move(push));
    wire::Receipt receipt;
    *receipt.mutable_epoch() = channel.epoch; receipt.set_channel_seq(*next_channel);
    receipt.set_client_send_seq(request.client_send_seq());
    auto text = request.text();
    *result.mutable_receipt() = receipt;
    result.set_outcome(wire::OUTCOME_ACCEPTED);
    member.state.mutable_last_receipt()->Swap(&receipt);
    member.state.set_last_client_send_seq(request.client_send_seq()); member.last_text.swap(text);
    member.tokens -= 1; channel.sequence = *next_channel;
    pushes_.swap(combined); push_bytes_ += bytes; ++stats_.accepted;
}
}  // namespace realm::game::chat
