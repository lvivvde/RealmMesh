#include "realmmesh/game/realm/realm_chat.hpp"
#include "chat_peer.hpp"
#include "realmmesh/game/realm/realm_scenes.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include <array>
#include <unordered_map>
#include <set>
#include <stdexcept>
namespace realm::game::realm {
namespace wire = ::realmmesh::protocol::chat::v1;
namespace {
using chat_detail::Clock;
using chat_detail::Peer;
constexpr auto timeout = std::chrono::milliseconds(common::chat_operation_timeout_ms);
bool same(const auto& a, const auto& b) { return a.SerializeAsString() == b.SerializeAsString(); }
wire::ChannelKind kind(std::size_t slot) { return slot == 0 ? wire::CHANNEL_KIND_REALM : wire::CHANNEL_KIND_SCENE; }
common::ChatState unavailable(wire::ChannelKind k, wire::Reason reason) {
    common::ChatState result; result.set_kind(k); result.set_phase(wire::PHASE_UNAVAILABLE); result.set_reason(reason); return result;
}
common::ChatSendResult result(const common::SendChat& send, wire::Outcome outcome, wire::Reason reason) {
    common::ChatSendResult value; value.set_kind(send.kind()); value.set_membership_generation(send.membership_generation());
    *value.mutable_epoch() = send.expected_epoch(); value.set_client_send_seq(send.client_send_seq());
    value.set_outcome(outcome); value.set_reason(reason);
    if (reason == wire::REASON_BUSY || reason == wire::REASON_RATE_LIMITED || reason == wire::REASON_RESOURCE_LIMIT)
        value.set_retry_after_ms(200);
    return value;
}
}
class RealmChat::State {
    struct Owner {
        std::unique_ptr<Peer> io;
        std::uint64_t revision{0}, generation{0};
    };
    struct Slot {
        Owner* owner{nullptr};
        wire::Membership member;
        common::ChatState state;
        bool active{false}, retiring{false}, busy{false};
        Clock::time_point next_attempt{};
        std::uint64_t watermark{0};
        std::deque<common::ChatMessage> buffered;
        std::size_t bytes{0};
        std::optional<common::SendChat> charged;
    };
    struct Session {
        edge::EdgeConnectionId id;
        std::uint64_t character{0};
        bool alive{true};
        std::array<Slot, 2> slots;
        double tokens{common::chat_send_burst};
        Clock::time_point refilled{Clock::now()};
        std::array<unsigned, 2> join_attempts{3, 3};
        std::array<std::uint64_t, 2> desired_channel{3, 3}, desired_epoch{0, 0};
    };
    struct Pending {
        std::shared_ptr<Session> session;
        std::size_t index;
        wire::Membership member;
        wire::Operation operation;
        std::optional<common::SendChat> send;
        std::uint64_t request_id;
        Owner* owner;
        std::uint64_t revision;
        Clock::time_point started;
        std::future<chat_detail::Completion> future;
    };
public:
    State(RealmOutbox& outbox, const RealmConfig& config, observability::MetricsRegistry& metrics, RealmScenes* scenes)
        : outbox_(outbox), enabled_(config.chat.enabled), metrics_(metrics), scenes_(scenes),
          incarnation_(common::correlation_id_hex(common::make_correlation_id())) {
        for (const auto* name : {"realm_chat_gap_total", "realm_chat_reconnect_total", "realm_chat_queue_rejections_total", "realm_chat_rate_rejections_total"})
            metrics_.counter_add(name, 0);
        for (const auto* outcome : {"accepted", "rejected", "unknown"}) metrics_.counter_add("realm_chat_send_total", 0, {{"outcome", outcome}});
        if (!enabled_) return;
        if (config.chat.targets.empty() || config.chat.targets.size() > common::chat_instances_max)
            throw std::invalid_argument("Realm Chat requires 1..2 targets");
        std::set<std::string> instances;
        for (const auto& target : config.chat.targets) {
            if (target.instance_id.empty() || target.instance_id.size() > 128 || target.server_identity.empty() ||
                target.host.empty() || target.port == 0 || target.channels.empty() || !instances.insert(target.instance_id).second)
                throw std::invalid_argument("invalid Realm Chat target");
            auto owner = std::make_unique<Owner>();
            owner->io = std::make_unique<Peer>(config.chat, target, incarnation_);
            for (const auto channel : target.channels) {
                if (channel > 2 || routes_[channel]) throw std::invalid_argument("Chat channel must have exactly one owner");
                routes_[channel] = owner.get();
            }
            owners_.push_back(std::move(owner));
        }
    }
    void selected(edge::EdgeConnectionId id, std::uint64_t character) {
        closed(id);
        if (!enabled_) return;
        if (sessions_.size() >= common::chat_characters_max) {
            if (deferred_.size() < common::chat_characters_max) deferred_.emplace(id, character);
            return;
        }
        auto session = std::make_shared<Session>(); session->id = id; session->character = character;
        // Session 顶替的待撤销资格继续占名额；不能跨会话复制资格。
        for (const auto& [key, existing] : sessions_) {
            static_cast<void>(key);
            if (existing->character == character) {
                if (deferred_.size() < common::chat_characters_max) deferred_.emplace(id, character);
                return;
            }
        }
        sessions_[id] = session;
        reconcile(*session);
    }
    void closed(edge::EdgeConnectionId id) {
        deferred_.erase(id);
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) return;
        found->second->alive = false;
        for (auto& slot : found->second->slots) retire(*found->second, slot, wire::REASON_STALE_MEMBERSHIP);
    }
    bool handle(edge::EdgeConnectionId id, std::span<const std::byte> payload) {
        const auto query = common::decode_get_chat_state(payload);
        auto send = common::decode_send_chat(payload);
        if (send) {
            // 客户端未知字段不进入内部线/幂等缓存；身份和正文均取受限的已知字段。
            common::SendChat canonical;
            canonical.set_kind(send->kind()); canonical.set_membership_generation(send->membership_generation());
            canonical.set_client_send_seq(send->client_send_seq()); canonical.set_text(send->text());
            auto* epoch = canonical.mutable_expected_epoch(); epoch->set_chat_incarnation(send->expected_epoch().chat_incarnation());
            auto* channel = epoch->mutable_channel(); channel->set_realm_id(send->expected_epoch().channel().realm_id());
            channel->set_kind(send->expected_epoch().channel().kind()); channel->set_shard_id(send->expected_epoch().channel().shard_id());
            send = std::move(canonical);
        }
        if (!query && !send) return false;
        const auto request = common::chat_request_id(payload).value_or(1);
        const auto k = query ? query->kind() : send->kind();
        const auto index = k == wire::CHANNEL_KIND_REALM ? std::size_t{0} : std::size_t{1};
        const auto found = sessions_.find(id);
        if (found == sessions_.end() || !found->second->alive) {
            const auto why = enabled_ ? wire::REASON_RESOURCE_LIMIT : wire::REASON_NOT_CONFIGURED;
            if (query) outbox_.send(id, common::encode(unavailable(k, why), request));
            else send_result(id, result(*send, wire::OUTCOME_REJECTED, why), request);
            return true;
        }
        auto session = found->second;
        if (query && !session->slots[index].owner) session->join_attempts[index] = 3;
        reconcile(*session); // 转场可在本帧前一个消息刚开始，不能等到帧尾。
        auto& slot = session->slots[index];
        if (!slot.active) {
            if (query) outbox_.send(id, common::encode(unavailable(k, slot.retiring ? wire::REASON_UNAVAILABLE : wire::REASON_NOT_MEMBER), request));
            else send_result(id, result(*send, wire::OUTCOME_REJECTED, wire::REASON_STALE_MEMBERSHIP), request);
            return true;
        }
        if (send && (send->membership_generation() != slot.member.generation() || !same(send->expected_epoch(), slot.state.epoch()))) {
            send_result(id, result(*send, wire::OUTCOME_REJECTED, wire::REASON_STALE_MEMBERSHIP), request); return true;
        }
        if (slot.busy) {
            if (query) outbox_.send(id, common::encode(unavailable(k, wire::REASON_BUSY), request));
            else send_result(id, result(*send, wire::OUTCOME_REJECTED, wire::REASON_BUSY), request);
            return true;
        }
        bool charged = false;
        if (send && (!slot.charged || !same(*slot.charged, *send))) {
            const auto now = Clock::now();
            session->tokens = std::min(double(common::chat_send_burst), session->tokens +
                std::chrono::duration<double>(now - session->refilled).count() * common::chat_send_rate_per_second);
            session->refilled = now;
            if (session->tokens < 1) {
                metrics_.counter_add("realm_chat_rate_rejections_total");
                send_result(id, result(*send, wire::OUTCOME_REJECTED, wire::REASON_RATE_LIMITED), request); return true;
            }
            session->tokens -= 1; charged = true;
        }
        if (!submit(session, index, query ? wire::OPERATION_STATE : wire::OPERATION_SEND, request, send)) {
            if (charged) session->tokens += 1;
            metrics_.counter_add("realm_chat_queue_rejections_total");
            if (query) outbox_.send(id, common::encode(unavailable(k, wire::REASON_BUSY), request));
            else send_result(id, result(*send, wire::OUTCOME_REJECTED, wire::REASON_BUSY), request);
        } else if (send) slot.charged = *send;
        return true;
    }
    void complete(std::size_t max) {
        // 先使旧链路和 Scene 资格失效，再收完成/推送。
        for (auto& owner : owners_) {
            const auto revision = owner->io->revision();
            if (owner->revision != revision) {
                owner->revision = revision; metrics_.counter_add("realm_chat_reconnect_total");
                for (auto& [id, session] : sessions_) {
                    static_cast<void>(id);
                    for (auto& slot : session->slots) if (slot.owner == owner.get()) {
                        retire(*session, slot, wire::REASON_UNAVAILABLE);
                        if (!slot.busy) clear(slot);
                    }
                }
            }
        }
        for (auto& [id, session] : sessions_) { static_cast<void>(id); reconcile(*session); }
        std::size_t count = 0;
        for (auto it = pending_.begin(); it != pending_.end() && count < std::min(max, common::chat_requests_per_round_max);) {
            if (it->future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
            auto p = std::move(*it); it = pending_.erase(it); ++count;
            auto completion = p.future.get(); p.owner->io->collected();
            finish(p, completion);
        }
        for (auto& owner : owners_) for (auto& push : owner->io->drain(common::chat_pushes_per_round_max)) {
            const auto found = sessions_.find(edge::EdgeConnectionId{push.membership().session_binding()});
            if (found == sessions_.end() || !found->second->alive) continue;
            auto& session = *found->second;
            auto& slot = session.slots[push.membership().channel().kind() == wire::CHANNEL_KIND_REALM ? 0 : 1];
            if (slot.owner != owner.get() || slot.retiring || !same(slot.member, push.membership())) continue;
            if (slot.active && !same(slot.state.epoch(), push.message().epoch())) { retire(session, slot, wire::REASON_WRONG_EPOCH); continue; }
            const auto bytes = common::encode(push.message()).size();
            const auto used = common::ChatBudget{session.slots[0].buffered.size() + session.slots[1].buffered.size(),
                session.slots[0].bytes + session.slots[1].bytes};
            if (!common::chat_budget_allows(used, {1, bytes}, {common::chat_client_notifications_max, common::chat_client_notification_bytes_max})) {
                metrics_.counter_add("realm_chat_gap_total"); retire(session, slot, wire::REASON_GAP); continue;
            }
            slot.bytes += bytes; slot.buffered.push_back(std::move(*push.mutable_message()));
        }
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            auto& s = *it->second;
            for (std::size_t index = 0; index < 2; ++index) {
                auto& slot = s.slots[index];
                if (slot.active && s.alive) {
                    while (!slot.buffered.empty()) {
                        auto message = std::move(slot.buffered.front()); slot.buffered.pop_front();
                        slot.bytes -= common::encode(message).size();
                        if (message.channel_seq() <= slot.watermark) continue;
                        if (!same(message.epoch(), slot.state.epoch()) || message.channel_seq() != slot.watermark + 1) {
                            metrics_.counter_add("realm_chat_gap_total"); retire(s, slot, wire::REASON_GAP); break;
                        }
                        slot.watermark = message.channel_seq();
                        outbox_.send(s.id, common::encode(message));
                    }
                }
                if (slot.retiring && !slot.busy) {
                    if (slot.owner->revision != slot.owner->io->revision()) clear(slot);
                    else if (Clock::now() >= slot.next_attempt)
                        static_cast<void>(submit(it->second, index, wire::OPERATION_LEAVE));
                }
            }
            if (!s.alive && !s.slots[0].owner && !s.slots[1].owner) it = sessions_.erase(it);
            else { reconcile(s); ++it; }
        }
        // 顶替不会同时占第三个资格；旧撤销/完成已结算后才安装新会话。
        auto waiting = std::move(deferred_); deferred_.clear();
        for (const auto& [id, character] : waiting) selected(id, character);
        std::size_t members = 0;
        for (const auto& [id, s] : sessions_) { static_cast<void>(id); for (const auto& slot : s->slots) if (slot.active) ++members; }
        metrics_.gauge_set("realm_chat_active_memberships", static_cast<double>(members));
        metrics_.gauge_set("realm_chat_pending_requests", static_cast<double>(pending_.size()));
    }
private:
    void send_result(edge::EdgeConnectionId id, const common::ChatSendResult& value, std::uint64_t request) {
        metrics_.counter_add("realm_chat_send_total", 1, {{"outcome", value.outcome() == wire::OUTCOME_ACCEPTED ? "accepted" : value.outcome() == wire::OUTCOME_UNKNOWN ? "unknown" : "rejected"}});
        outbox_.send(id, common::encode(value, request));
    }
    void clear(Slot& slot) {
        const auto next = Clock::now() + std::chrono::milliseconds(250);
        slot = Slot{}; slot.next_attempt = next;
    }
    void retire(Session& session, Slot& slot, wire::Reason why) {
        if (!slot.owner) return;
        if (!slot.retiring && session.alive) {
            common::ChatUnavailable notice; notice.set_kind(slot.member.channel().kind());
            notice.set_reason(why); notice.set_rejoin_required(true);
            outbox_.send(session.id, common::encode(notice));
        }
        slot.active = false; slot.retiring = true; slot.buffered.clear(); slot.bytes = 0;
    }
    void reconcile(Session& session) {
        const auto scene = scenes_ ? scenes_->chat_binding(session.id) : std::nullopt;
        for (std::size_t index = 0; index < 2; ++index) {
            auto& slot = session.slots[index];
            const auto channel = index == 0 ? 0 : scene ? scene->shard_id : 3;
            auto* owner = channel < routes_.size() ? routes_[channel] : nullptr;
            const auto epoch = index == 0 ? 0 : scene ? scene->binding_epoch : 0;
            if (session.desired_channel[index] != channel || session.desired_epoch[index] != epoch) {
                session.desired_channel[index] = channel; session.desired_epoch[index] = epoch;
                session.join_attempts[index] = 3;
            }
            if (slot.owner && (!session.alive || slot.owner != owner || slot.member.channel().shard_id() != channel ||
                slot.member.scene_binding_epoch() != epoch)) retire(session, slot, wire::REASON_STALE_MEMBERSHIP);
            if (!session.alive || slot.owner || !owner || Clock::now() < slot.next_attempt) continue;
            if (owner->generation >= common::chat_counter_max || session.join_attempts[index] == 0) continue;
            --session.join_attempts[index];
            slot.owner = owner;
            auto& member = slot.member;
            member.mutable_channel()->set_realm_id(1); member.mutable_channel()->set_kind(kind(index));
            member.mutable_channel()->set_shard_id(channel); member.set_character_id(session.character);
            member.set_realm_incarnation(incarnation_); member.set_session_binding(session.id.value);
            member.set_generation(++owner->generation); member.set_scene_binding_epoch(epoch);
            if (!submit(sessions_.at(session.id), index, wire::OPERATION_JOIN)) clear(slot);
        }
    }
    bool submit(const std::shared_ptr<Session>& session, std::size_t index, wire::Operation operation,
        std::uint64_t request = 0, std::optional<common::SendChat> send = {}) {
        auto& slot = session->slots[index];
        if (!slot.owner || slot.busy) return false;
        common::ChatInternalRequest value; value.set_operation(operation); *value.mutable_membership() = slot.member;
        if (send) *value.mutable_send() = *send;
        const auto now = Clock::now();
        auto future = slot.owner->io->submit(std::move(value), now + timeout);
        if (!future) return false;
        pending_.push_back({session, index, slot.member, operation, std::move(send), request,
            slot.owner, slot.owner->io->revision(), now, std::move(*future)});
        slot.busy = true; return true;
    }
    void finish(const Pending& p, const chat_detail::Completion& completed) {
        const auto operation = p.operation == wire::OPERATION_JOIN ? "join" : p.operation == wire::OPERATION_LEAVE ? "leave" : p.operation == wire::OPERATION_SEND ? "send" : "state";
        metrics_.histogram_observe("realm_chat_operation_seconds", std::chrono::duration<double>(Clock::now() - p.started).count(), {{"operation", operation}});
        auto& s = *p.session; auto& slot = s.slots[p.index];
        slot.busy = false;
        const bool current = same(slot.member, p.member) && p.revision == p.owner->io->revision();
        const auto* reply = completed.reply ? &*completed.reply : nullptr;
        const bool ok = current && reply && reply->status() == wire::STATUS_OK;
        if (p.send && s.alive) {
            auto value = ok && reply->has_result() ? reply->result() : result(*p.send,
                !reply && completed.dispatched ? wire::OUTCOME_UNKNOWN : wire::OUTCOME_REJECTED,
                !reply && completed.dispatched ? wire::REASON_OUTCOME_LOST : wire::REASON_UNAVAILABLE);
            send_result(s.id, value, p.request_id);
            if (ok && value.outcome() == wire::OUTCOME_ACCEPTED) {
                slot.state.set_last_client_send_seq(value.client_send_seq());
                *slot.state.mutable_last_receipt() = value.receipt();
            }
        }
        if (!current) { clear(slot); return; }
        if (p.operation == wire::OPERATION_LEAVE) {
            if (ok) clear(slot);
            else { slot.owner->io->invalidate(); slot.next_attempt = Clock::now() + timeout; }
            return;
        }
        if (!ok) {
            if (p.request_id && !p.send && s.alive) outbox_.send(s.id, common::encode(unavailable(kind(p.index), wire::REASON_UNAVAILABLE), p.request_id));
            retire(s, slot, wire::REASON_UNAVAILABLE);
            if (!reply) clear(slot);
            return;
        }
        if (slot.retiring || !s.alive) return;
        if (reply->has_state()) {
            if (p.operation == wire::OPERATION_JOIN) slot.watermark = reply->state().join_watermark();
            else if (!same(reply->state().epoch(), slot.state.epoch()) || reply->state().join_watermark() != slot.state.join_watermark()) {
                retire(s, slot, wire::REASON_WRONG_EPOCH); return;
            }
            slot.state = reply->state(); slot.active = true;
            if (p.request_id) outbox_.send(s.id, common::encode(slot.state, p.request_id));
        }
    }
    RealmOutbox& outbox_;
    bool enabled_;
    observability::MetricsRegistry& metrics_;
    RealmScenes* scenes_;
    std::string incarnation_;
    std::vector<std::unique_ptr<Owner>> owners_;
    std::array<Owner*, 3> routes_{};
    std::unordered_map<edge::EdgeConnectionId, std::shared_ptr<Session>> sessions_;
    std::vector<Pending> pending_;
    std::unordered_map<edge::EdgeConnectionId, std::uint64_t> deferred_;
};
RealmChat::RealmChat(RealmOutbox& outbox, const RealmConfig& config, observability::MetricsRegistry& metrics, RealmScenes* scenes)
    : state_(std::make_unique<State>(outbox, config, metrics, scenes)) {}
RealmChat::~RealmChat() = default;
void RealmChat::selected(edge::EdgeConnectionId id, std::uint64_t character) { state_->selected(id, character); }
void RealmChat::closed(edge::EdgeConnectionId id) { state_->closed(id); }
bool RealmChat::handle(edge::EdgeConnectionId id, std::span<const std::byte> payload) { return state_->handle(id, payload); }
void RealmChat::complete(std::size_t max) { state_->complete(max); }
}
