#include "realmmesh/game/realm/realm_scenes.hpp"

#include <deque>
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/scene_internal_protocol.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/network/client/tls_client_stream.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "scene_async.hpp"
#include "scene_peer.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>

namespace realm::game::realm {
namespace internal = ::realmmesh::protocol::scene_internal::v1;
namespace wire = ::realmmesh::protocol::scene::v1;
namespace {
using Clock = std::chrono::steady_clock;
constexpr auto scene_operation_timeout = std::chrono::seconds(3);
using scene_detail::Failure;
using scene_detail::Task;
wire::Location location(const common::SceneLocation& value) {
    wire::Location result;
    result.set_shard_id(value.shard_id);
    result.set_x(value.x);
    result.set_y(value.y);
    return result;
}
common::SceneCommandResult result(
    const common::SceneStoredResult& value, bool replayed) {
    common::SceneCommandResult message;
    message.set_kind(static_cast<wire::CommandKind>(value.command.type));
    message.set_scene_seq(value.command.seq);
    *message.mutable_location() = location(value.location);
    message.set_binding_epoch(value.binding_epoch);
    message.set_replayed(replayed);
    return message;
}
std::string digest(const common::SceneCommand& command) {
    const auto hex = common::scene_command_digest(command);
    std::string bytes;
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        bytes.push_back(
            static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}
}  // namespace

class RealmScenes::Worker final {
    struct PeerSlot {
        std::unique_ptr<scene_detail::Peer> peer;
        std::atomic_size_t in_flight{0};
        std::uint64_t generation{0};
        bool health_busy{false};
        Clock::time_point next_health{Clock::now()};
        bool ready{false}, draining{false}, drain_failed{false};
        std::map<std::uint64_t, internal::ShardStatus> hosted;
    };

public:
    enum class Stage { Queued, Reading, Writing, Finished, Cancelled };
    struct Control {
        std::uint64_t request_id{0};
        Clock::time_point deadline;
        std::atomic<Stage> stage{Stage::Queued};
        std::atomic_bool timed_out{false};
        bool transfer{false};
    };
    struct Session;
    struct Job {
        std::shared_ptr<Session> session;
        std::uint64_t request_id;
        std::shared_ptr<Control> control;
        std::optional<common::SceneCommand> command;
        Clock::time_point submitted, deadline;
        PeerSlot* budget;
        bool drain_transfer{false};
    };
    struct Session {
        edge::EdgeConnectionId id;
        common::SceneCharacterKey character;
        std::uint64_t binding{0};
        std::atomic_bool alive{true};
        std::atomic_uint64_t routed_shard{1};
        std::size_t outstanding{0};
        std::shared_ptr<const RealmScenes::ChatBinding> chat_binding;
        std::atomic_size_t transfers{0};
        std::vector<std::shared_ptr<Control>> pending;  // 仅帧线程。
        // 以下只由场景编排线程访问；同一会话一次只恢复一个指令协程。
        Clock::time_point retire_after{Clock::now()};
        std::uint64_t peer_generation{0};
        bool busy{false}, cleaning{false}, reclaimed{false}, retiring{false};
        std::deque<Job> waiting;
        std::optional<common::SceneStateRecord> record, pending_source,
            last_owned;
        std::optional<common::SceneWrite> unresolved;
        std::map<std::uint64_t, common::SceneStateRecord> retirements;
        bool active{false}, confirmation_pending{false}, resync{false},
            snapshot_required{false};
        wire::Snapshot snapshot;
        PeerSlot* drain_origin{nullptr};
        std::optional<common::SceneCommand> drain_command;
        Clock::time_point next_drain{Clock::now()};
    };
    struct Output {
        std::shared_ptr<Session> session;
        std::vector<std::byte> payload;
        bool done{false};
        std::shared_ptr<Control> control;
        PeerSlot* budget{nullptr};
    };
    Worker(
        common::RealmSceneStore& store,
        RealmOutbox& outbox,
        const RealmConfig& config,
        observability::MetricsRegistry& metrics,
        common::SceneShardStore* shards)
        : store_(store),
          outbox_(outbox),
          metrics_(metrics),
          shard_store_(shards),
          dynamic_(config.scene.enabled && config.scene.dynamic),
          incarnation_(
              common::correlation_id_hex(common::make_correlation_id())) {
        if (config.scene.enabled) {
            for (const auto* name : {"scene_queue_rejections_total",
                                     "scene_stale_rejections_total",
                                     "scene_snapshot_resync_total",
                                     "scene_write_uncertain_total"})
                metrics_.counter_add(name, 0);
        }
        if (dynamic_ && !shard_store_)
            throw std::invalid_argument("dynamic Scene requires shard store");
        if (dynamic_) {
            metrics_.counter_add("scene_shard_reconcile_errors_total", 0);
            metrics_.counter_add("scene_drain_transfers_total", 0);
            for (const auto* operation : {"assign", "recover", "release"})
                metrics_.counter_add(
                    "scene_shard_assignments_total",
                    0,
                    {{"operation", operation}});
            for (const auto* reason : {"capacity", "unavailable"})
                metrics_.counter_add(
                    "scene_drain_retries_total", 0, {{"reason", reason}});
        }
        RealmConfig::SceneTarget first;
        first.shard_id = 1;
        first.host = config.scene.host;
        first.port = config.scene.port;
        first.instance_id = config.scene.instance_id;
        first.server_identity = config.scene.server_identity;
        auto targets = config.scene.targets;
        targets.insert(targets.begin(), first);
        if (targets.size() > 2)
            throw std::invalid_argument(
                "S3 permits two configured Scene shards");
        for (const auto& target : targets) {
            if (target.shard_id == 0 || target.shard_id > 2 ||
                peers_.contains(target.shard_id))
                throw std::invalid_argument("duplicate Scene shard");
            for (const auto& [id, slot] : peers_) {
                static_cast<void>(id);
                if (slot->peer->target.instance_id == target.instance_id)
                    throw std::invalid_argument(
                        "Scene shards need independent instances");
            }
            auto slot = std::make_unique<PeerSlot>();
            slot->peer = std::make_unique<scene_detail::Peer>(
                config.scene, target, incarnation_, pushes_);
            peers_.emplace(target.shard_id, std::move(slot));
            targets_.emplace(target.shard_id, target);
        }
        thread_ = std::jthread([this](std::stop_token stop) {
            run(stop);
        });
    }
    ~Worker() {
        for (auto& [id, session] : sessions_) {
            static_cast<void>(id);
            session->alive = false;
        }
        thread_.request_stop();
        thread_.join();
    }
    void selected(
        edge::EdgeConnectionId id, common::SceneCharacterKey character) {
        closed(id);
        auto session = std::make_shared<Session>();
        session->id = id;
        session->character = character;
        session->binding = next_binding_++;
        sessions_[id] = std::move(session);
    }
    void closed(edge::EdgeConnectionId id) {
        const auto found = sessions_.find(id);
        if (found != sessions_.end()) {
            found->second->alive = false;
            sessions_.erase(found);
        }
    }
    bool handle(edge::EdgeConnectionId id, std::span<const std::byte> payload) {
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) return false;
        std::optional<common::SceneCommand> command;
        if (const auto enter = common::decode_enter_scene(payload))
            command = common::SceneCommand{
                common::SceneCommandType::Enter, enter->scene_seq()};
        else if (const auto move = common::decode_move_in_scene(payload))
            command = common::SceneCommand{
                common::SceneCommandType::Move,
                move->scene_seq(),
                move->x(),
                move->y()};
        else if (const auto transfer = common::decode_transfer_scene(payload))
            command = common::SceneCommand{
                common::SceneCommandType::Transfer,
                transfer->scene_seq(),
                0,
                0,
                transfer->target_shard_id()};
        else if (!common::decode_get_scene_state(payload))
            return false;
        const auto request_id = common::scene_request_id(payload).value_or(0);
        const auto session = found->second;
        const auto shard =
            command && command->type == common::SceneCommandType::Transfer
                ? command->target_shard_id
                : session->routed_shard.load();
        if (!targets_.contains(shard)) {
            outbox_.send(
                id, error(request_id, common::scene_error_invalid_target));
            return true;
        }
        demand_[static_cast<std::size_t>(shard - 1)] = true;
        const auto destination =
            peers_.find(routes_[static_cast<std::size_t>(shard - 1)].load());
        auto* budget = destination->second.get();
        if (session->outstanding >= 9 || !reserve(*budget)) {
            outbox_.send(id, error(request_id, common::edge_error_throttled));
            metrics_.counter_add("scene_queue_rejections_total");
            return true;
        }
        auto control = std::make_shared<Control>();
        control->request_id = request_id;
        control->transfer =
            command && command->type == common::SceneCommandType::Transfer;
        const auto now = Clock::now();
        control->deadline = now + scene_operation_timeout;
        if (!jobs_.try_push(
                {session,
                 request_id,
                 control,
                 command,
                 now,
                 control->deadline,
                 budget})) {
            --budget->in_flight;
            outbox_.send(id, error(request_id, common::edge_error_throttled));
            metrics_.counter_add("scene_queue_rejections_total");
            return true;
        }
        if (command && command->type == common::SceneCommandType::Transfer)
            ++session->transfers;
        ++session->outstanding;
        session->pending.push_back(std::move(control));
        return true;
    }
    void complete(std::size_t max) {
        for (auto& output : outputs_.drain(max)) {
            if (output.done) {
                --output.session->outstanding;
                if (output.control && output.control->transfer)
                    --output.session->transfers;
                --output.budget->in_flight;
                std::erase(output.session->pending, output.control);
            }
            const auto found = sessions_.find(output.session->id);
            if (found != sessions_.end() && found->second == output.session &&
                output.session->alive && !output.payload.empty() &&
                (!output.control || !output.control->timed_out))
                outbox_.send(output.session->id, std::move(output.payload));
        }
        for (auto it = sessions_.begin(); it != sessions_.end();) {
            for (const auto& pending : it->second->pending) {
                if (pending->timed_out || Clock::now() < pending->deadline)
                    continue;
                auto stage = pending->stage.load();
                while ((stage == Stage::Queued || stage == Stage::Reading) &&
                       !pending->stage.compare_exchange_weak(
                           stage, Stage::Cancelled)) {
                }
                if (stage == Stage::Finished) continue;
                pending->timed_out = true;
                outbox_.send(
                    it->first,
                    error(
                        pending->request_id,
                        stage == Stage::Writing
                            ? common::scene_error_outcome_unknown
                            : common::scene_error_unavailable));
            }
            if (!it->second->alive) {
                outbox_.close(it->first);
                it = sessions_.erase(it);
            } else
                ++it;
        }
    }
    void endpoint(
        const std::optional<network::TransportEndpoint>& value,
        std::string_view instance) {
        for (auto& [id, slot] : peers_) {
            if ((instance.empty() && id == 1) ||
                slot->peer->target.instance_id == instance)
                slot->peer->endpoint(value);
        }
    }
    void discovery_ready(bool ready) { discovery_ready_ = ready; }
    std::optional<RealmScenes::ChatBinding> chat_binding(
        edge::EdgeConnectionId id) const {
        const auto found = sessions_.find(id);
        if (found == sessions_.end() || !found->second->alive ||
            found->second->transfers != 0)
            return {};
        const auto value = std::atomic_load(&found->second->chat_binding);
        if (!value) return {};
        return *value;
    }

private:
    static bool reserve(PeerSlot& slot) {
        auto value = slot.in_flight.load();
        while (value < 64) {
            if (slot.in_flight.compare_exchange_weak(value, value + 1))
                return true;
        }
        return false;
    }
    PeerSlot* instance(std::string_view id) {
        for (auto& [key, slot] : peers_) {
            static_cast<void>(key);
            if (slot->peer->target.instance_id == id) return slot.get();
        }
        return nullptr;
    }
    PeerSlot& peer(std::uint64_t shard) {
        if (!targets_.contains(shard))
            throw Failure{common::scene_error_invalid_target};
        if (dynamic_ &&
            (!ownership_[static_cast<std::size_t>(shard - 1)] ||
             !ownership_[static_cast<std::size_t>(shard - 1)]->owner))
            throw Failure{common::scene_error_unavailable};
        const auto found =
            peers_.find(routes_[static_cast<std::size_t>(shard - 1)].load());
        return *found->second;
    }
    PeerSlot& peer(const common::SceneStateRecord& record) {
        auto* slot =
            record.owner ? instance(record.owner->scene_instance_id) : nullptr;
        if (!slot) throw Failure{common::scene_error_stale_binding};
        return *slot;
    }
    template <class F>
    auto database(F action) {
        if (!database_)
            database_ = std::make_unique<scene_detail::DatabaseJobs>();
        return scheduler_.wait(database_->submit(std::move(action)));
    }
    static std::vector<std::byte> error(std::uint64_t id, int code) {
        common::EdgeError value;
        value.set_code(static_cast<std::uint32_t>(code));
        value.set_message("scene request unavailable or rejected");
        if (code == common::edge_error_throttled)
            value.set_retry_after_seconds(1);
        return common::encode(value, id);
    }
    void output(
        const std::shared_ptr<Session>& session,
        std::vector<std::byte> payload,
        bool done = false,
        std::shared_ptr<Control> control = {},
        PeerSlot* budget = nullptr) {
        if (!outputs_.try_push(
                {session,
                 std::move(payload),
                 done,
                 std::move(control),
                 budget})) {
            session->alive = false;
            if (done) --budget->in_flight;
        }
    }
    bool owned(const Session& session) const {
        return session.record && session.record->owner &&
               session.record->owner->realm_incarnation == incarnation_ &&
               session.record->owner->session_binding == session.binding;
    }
    void unavailable(const std::shared_ptr<Session>& session) {
        if (!session->active) return;
        std::atomic_store(
            &session->chat_binding,
            std::shared_ptr<const RealmScenes::ChatBinding>{});
        session->active = false;
        common::SceneUnavailable notice;
        notice.set_shard_id(session->record->confirmed_location->shard_id);
        notice.set_binding_epoch(session->record->binding_epoch);
        notice.set_retry_after_seconds(1);
        output(session, common::encode(notice));
    }
    common::SceneRequest request(
        PeerSlot& slot, internal::Operation operation) const {
        common::SceneRequest value;
        value.set_operation(operation);
        value.set_realm_incarnation(incarnation_);
        value.set_scene_instance_id(slot.peer->target.instance_id);
        value.set_scene_incarnation(slot.peer->incarnation());
        return value;
    }
    common::SceneRequest routed(
        internal::Operation operation, const common::SceneStateRecord& record) {
        auto value = request(peer(record), operation);
        value.set_scene_incarnation(record.owner->scene_incarnation);
        auto* route = value.mutable_route();
        route->set_realm_id(record.character.realm_id);
        route->set_character_id(record.character.character_id);
        route->set_binding_epoch(record.binding_epoch);
        route->set_session_binding(record.owner->session_binding);
        route->set_shard_id(record.confirmed_location->shard_id);
        route->set_shard_version(record.owner->shard_version);
        return value;
    }
    auto exchange(
        PeerSlot& slot,
        common::SceneRequest message,
        Clock::time_point deadline) {
        return scheduler_.wait(
            slot.peer->exchange(std::move(message), deadline));
    }
    Task<> connect(PeerSlot& slot, Clock::time_point deadline) {
        if (!slot.peer->incarnation().empty()) co_return;
        auto probe = request(
            slot,
            dynamic_ ? internal::OPERATION_STATUS
                     : internal::OPERATION_SNAPSHOT);
        if (!dynamic_) probe.set_shard_id(slot.peer->target.shard_id);
        static_cast<void>(co_await exchange(slot, std::move(probe), deadline));
    }
    common::SceneState state(const Session& session) const {
        common::SceneState value;
        value.set_phase(
            session.active ? wire::PHASE_ACTIVE
            : session.record && session.record->confirmed_location
                ? wire::PHASE_UNAVAILABLE
                : wire::PHASE_OUTSIDE);
        if (session.record) {
            value.set_binding_epoch(session.record->binding_epoch);
            if (session.record->confirmed_location)
                *value.mutable_confirmed_location() =
                    location(*session.record->confirmed_location);
            if (session.record->last_result) {
                value.set_last_scene_seq(
                    session.record->last_result->command.seq);
                *value.mutable_last_result() =
                    result(*session.record->last_result, true);
            }
        }
        if (session.active) *value.mutable_snapshot() = session.snapshot;
        return value;
    }
    Task<> snapshot(Session& session, Clock::time_point deadline) {
        auto& slot = peer(*session.record);
        auto probe = request(slot, internal::OPERATION_SNAPSHOT);
        probe.set_shard_id(session.record->confirmed_location->shard_id);
        probe.set_shard_version(session.record->owner->shard_version);
        const auto reply = co_await exchange(slot, std::move(probe), deadline);
        bool contains = false;
        for (const auto& member : reply.snapshot().members())
            if (member.character_id() == session.character.character_id &&
                member.binding_epoch() == session.record->binding_epoch)
                contains = true;
        if (!contains) throw Failure{common::scene_error_stale_binding};
        session.snapshot = reply.snapshot();
        session.resync = false;
    }
    Task<> restore(Session& session, Clock::time_point deadline) {
        if (!owned(session)) throw Failure{common::scene_error_stale_binding};
        auto& slot = peer(*session.record);
        co_await connect(slot, deadline);
        if (session.record->owner->scene_incarnation !=
            slot.peer->incarnation())
            throw Failure{common::scene_error_stale_binding};
        const auto receipt = *session.record->last_result;
        auto prepare = routed(internal::OPERATION_PREPARE, *session.record);
        prepare.set_scene_seq(receipt.command.seq);
        prepare.set_command_digest(digest(receipt.command));
        *prepare.mutable_location() = location(receipt.location);
        static_cast<void>(
            co_await exchange(slot, std::move(prepare), deadline));
        auto activate = routed(internal::OPERATION_ACTIVATE, *session.record);
        activate.set_scene_seq(receipt.command.seq);
        activate.set_command_digest(digest(receipt.command));
        session.snapshot =
            (co_await exchange(slot, std::move(activate), deadline)).snapshot();
        session.active = true;
        std::atomic_store(
            &session.chat_binding,
            std::make_shared<const RealmScenes::ChatBinding>(
                RealmScenes::ChatBinding{
                    session.record->confirmed_location->shard_id,
                    session.record->binding_epoch}));
        session.peer_generation = slot.peer->generation();
        session.resync = false;
    }
    void committed(Session& session, const common::SceneStateRecord& value) {
        session.record = value;
        session.last_owned = value;
        session.unresolved.reset();
        session.confirmation_pending = true;
        session.routed_shard = value.confirmed_location->shard_id;
        if (session.pending_source && session.pending_source->owner) {
            session.retirements.insert_or_assign(
                session.pending_source->confirmed_location->shard_id,
                *session.pending_source);
            session.pending_source.reset();
        }
    }
    Task<common::SceneWriteResult> commit(
        Session& session,
        common::SceneWrite write,
        Clock::time_point deadline) {
        session.unresolved = write;
        std::optional<common::SceneWriteResult> value;
        try {
            auto pending = database([this, write] {
                return store_.commit_scene(write);
            });
            value = co_await pending;
        } catch (const common::SceneWriteUncertain&) {
            metrics_.counter_add("scene_write_uncertain_total");
        }
        if (value) {
            session.unresolved.reset();
            co_return std::move(*value);
        }
        do {
            auto pending = database([this, key = session.character] {
                return store_.scene_state(key);
            });
            auto observed = co_await pending;
            if (common::resolve_scene_write(write, observed) ==
                common::SceneWriteResolution::Committed) {
                session.unresolved.reset();
                co_return common::SceneWriteResult{
                    common::SceneWriteOutcome::Committed, std::move(observed)};
            }
        } while (Clock::now() < deadline && session.alive);
        throw Failure{common::scene_error_outcome_unknown};
    }
    Task<> finish(
        const Job& job, common::SceneWriteResult value, bool replayed) {
        auto& session = *job.session;
        if (value.outcome != common::SceneWriteOutcome::Committed &&
            value.outcome != common::SceneWriteOutcome::Replayed) {
            session.pending_source.reset();
            if (value.outcome == common::SceneWriteOutcome::InvalidSequence)
                throw Failure{common::scene_error_invalid_seq};
            throw Failure{common::scene_error_stale_binding};
        }
        committed(session, *value.state);
        if (!session.alive) co_return;
        try {
            if (job.command->type == common::SceneCommandType::Move &&
                !replayed) {
                auto& slot = peer(*session.record);
                auto confirm =
                    routed(internal::OPERATION_CONFIRM, *session.record);
                confirm.set_scene_seq(job.command->seq);
                confirm.set_command_digest(digest(*job.command));
                *confirm.mutable_location() =
                    location(*session.record->confirmed_location);
                static_cast<void>(
                    co_await exchange(slot, std::move(confirm), job.deadline));
            } else {
                std::atomic_store(
                    &session.chat_binding,
                    std::shared_ptr<const RealmScenes::ChatBinding>{});
                session.active = false;
                co_await restore(session, job.deadline);
            }
        } catch (...) {
            unavailable(job.session);
            throw Failure{common::scene_error_outcome_unknown};
        }
        session.confirmation_pending = false;
        if (job.drain_transfer) co_return;
        output(
            job.session,
            common::encode(
                result(
                    *session.record->last_result,
                    replayed ||
                        value.outcome == common::SceneWriteOutcome::Replayed),
                job.request_id),
            false,
            job.control);
    }
    Task<> execute(Job& job) {
        auto& session = *job.session;
        if (!session.alive) co_return;
        Stage queued = Stage::Queued;
        if (!job.control->stage.compare_exchange_strong(
                queued, Stage::Reading) ||
            Clock::now() >= job.deadline)
            throw Failure{common::scene_error_unavailable};
        auto pending = database([this, key = session.character] {
            return store_.initialize_scene(key);
        });
        session.record = co_await pending;
        if (!session.record) throw Failure{common::scene_error_stale_binding};
        if (session.record->confirmed_location)
            session.routed_shard = session.record->confirmed_location->shard_id;
        if (session.record->confirmed_location &&
            targets_.contains(session.record->confirmed_location->shard_id))
            demand_[static_cast<std::size_t>(
                session.record->confirmed_location->shard_id - 1)] = true;
        if (!owned(session) && session.active) unavailable(job.session);
        if (session.unresolved) {
            if (common::resolve_scene_write(
                    *session.unresolved, session.record) ==
                common::SceneWriteResolution::Committed) {
                committed(session, *session.record);
            } else if (
                job.command && *job.command == session.unresolved->command) {
                Stage reading = Stage::Reading;
                if (!job.control->stage.compare_exchange_strong(
                        reading, Stage::Writing))
                    throw Failure{common::scene_error_unavailable};
                const auto write = *session.unresolved;
                auto settled = co_await commit(session, write, job.deadline);
                co_await finish(job, std::move(settled), true);
                co_return;
            } else
                throw Failure{common::scene_error_outcome_unknown};
        }
        if (!job.command) {
            if (owned(session) && session.confirmation_pending) {
                bool stale = false;
                try {
                    co_await restore(session, job.deadline);
                    session.confirmation_pending = false;
                } catch (const Failure& failure) {
                    stale = failure.code == common::scene_error_stale_binding;
                    unavailable(job.session);
                }
                if (stale) session.confirmation_pending = false;
            }
            if (session.active) {
                try {
                    co_await snapshot(session, job.deadline);
                } catch (...) {
                    unavailable(job.session);
                }
            }
            session.snapshot_required = false;
            if (job.drain_transfer) co_return;
            output(
                job.session,
                common::encode(state(session), job.request_id),
                false,
                job.control);
            co_return;
        }
        const auto command = *job.command;
        const auto previous = session.record->last_result;
        const bool replayed = previous && previous->command == command;
        if (previous && command.seq <= previous->command.seq && !replayed)
            throw Failure{common::scene_error_invalid_seq};
        if (!replayed &&
            command.seq != (previous ? previous->command.seq + 1 : 1))
            throw Failure{common::scene_error_invalid_seq};
        if (replayed) {
            if (owned(session)) {
                bool stale = false;
                try {
                    co_await restore(session, job.deadline);
                    session.confirmation_pending = false;
                } catch (const Failure& failure) {
                    unavailable(job.session);
                    stale = failure.code == common::scene_error_stale_binding;
                    if (!stale)
                        throw Failure{common::scene_error_outcome_unknown};
                }
                if (stale) session.confirmation_pending = false;
            }
            if (!job.drain_transfer)
                output(
                    job.session,
                    common::encode(result(*previous, true), job.request_id),
                    false,
                    job.control);
            co_return;
        }
        if (session.confirmation_pending) {
            bool stale = false;
            try {
                co_await restore(session, job.deadline);
                session.confirmation_pending = false;
            } catch (const Failure& failure) {
                unavailable(job.session);
                stale = failure.code == common::scene_error_stale_binding;
                if (!stale) throw Failure{common::scene_error_outcome_unknown};
            }
            if (stale) session.confirmation_pending = false;
        }
        if (command.type != common::SceneCommandType::Enter &&
            ((!session.active && !job.drain_transfer) || !owned(session)))
            throw Failure{common::scene_error_not_active};
        const auto shard = command.type == common::SceneCommandType::Transfer
                               ? command.target_shard_id
                           : session.record->confirmed_location
                               ? session.record->confirmed_location->shard_id
                               : 1;
        auto& slot = peer(shard);
        if (job.budget != &slot) {
            if (!reserve(slot)) throw Failure{common::edge_error_throttled};
            --job.budget->in_flight;
            job.budget = &slot;
        }
        if (command.type == common::SceneCommandType::Transfer &&
            shard == session.record->confirmed_location->shard_id)
            throw Failure{common::scene_error_invalid_target};
        if (!dynamic_ && command.type == common::SceneCommandType::Enter &&
            session.record->owner &&
            session.record->owner->scene_instance_id !=
                slot.peer->target.instance_id)
            throw Failure{common::scene_error_stale_binding};
        co_await connect(slot, job.deadline);
        common::SceneWrite write;
        write.character = session.character;
        write.command = command;
        write.expected_epoch = session.record->binding_epoch;
        write.expected_owner = session.record->owner;
        write.owner = {
            incarnation_,
            session.binding,
            slot.peer->target.instance_id,
            slot.peer->incarnation()};
        if (dynamic_) {
            const auto& shard_owner =
                ownership_[static_cast<std::size_t>(shard - 1)];
            if (!shard_owner || !shard_owner->owner ||
                shard_owner->owner->instance_id !=
                    slot.peer->target.instance_id ||
                shard_owner->owner->incarnation != slot.peer->incarnation())
                throw Failure{common::scene_error_unavailable};
            write.owner.shard_version = shard_owner->version;
        }
        write.location = command.type == common::SceneCommandType::Enter
            ? session.record->confirmed_location.value_or(common::SceneLocation{1, 0, 0})
            : command.type == common::SceneCommandType::Transfer
                ? common::SceneLocation{shard, targets_.at(shard).entry_x, targets_.at(shard).entry_y}
                : common::SceneLocation{shard, command.x, command.y};
        if (session.record->binding_epoch >= common::scene_counter_max)
            throw Failure{common::scene_error_stale_binding};
        auto candidate = *session.record;
        candidate.owner = write.owner;
        candidate.confirmed_location = write.location;
        if (command.type != common::SceneCommandType::Move)
            ++candidate.binding_epoch;
        if (command.type == common::SceneCommandType::Move &&
            session.record->owner != std::optional(write.owner))
            throw Failure{common::scene_error_stale_binding};
        auto proposal = routed(
            command.type == common::SceneCommandType::Move
                ? internal::OPERATION_PROPOSE_MOVE
                : internal::OPERATION_PREPARE,
            candidate);
        proposal.set_scene_seq(command.seq);
        proposal.set_command_digest(digest(command));
        *proposal.mutable_location() = location(write.location);
        const auto proposed =
            co_await exchange(slot, std::move(proposal), job.deadline);
        if (proposed.location().SerializeAsString() !=
            location(write.location).SerializeAsString())
            throw Failure{common::scene_error_stale_binding};
        if (!session.alive || Clock::now() >= job.deadline)
            throw Failure{common::scene_error_unavailable};
        Stage reading = Stage::Reading;
        if (!job.control->stage.compare_exchange_strong(
                reading, Stage::Writing))
            throw Failure{common::scene_error_unavailable};
        if (command.type != common::SceneCommandType::Move && owned(session))
            session.pending_source = session.record;
        auto value = co_await commit(session, std::move(write), job.deadline);
        co_await finish(job, std::move(value), false);
    }
    Task<> process(Job job) {
        if (job.command &&
            job.command->type == common::SceneCommandType::Transfer)
            std::atomic_store(
                &job.session->chat_binding,
                std::shared_ptr<const RealmScenes::ChatBinding>{});
        int code = 0;
        try {
            co_await execute(job);
        } catch (const Failure& failure) {
            code = failure.code;
        } catch (...) {
            code = job.control->stage == Stage::Writing
                       ? common::scene_error_outcome_unknown
                       : common::scene_error_unavailable;
        }
        if (job.session->alive && job.session->active &&
            !job.session->unresolved && !job.session->confirmation_pending &&
            owned(*job.session))
            std::atomic_store(
                &job.session->chat_binding,
                std::make_shared<const RealmScenes::ChatBinding>(
                    RealmScenes::ChatBinding{
                        job.session->record->confirmed_location->shard_id,
                        job.session->record->binding_epoch}));
        else
            std::atomic_store(
                &job.session->chat_binding,
                std::shared_ptr<const RealmScenes::ChatBinding>{});
        if (code == common::scene_error_stale_binding)
            metrics_.counter_add("scene_stale_rejections_total");
        if (job.drain_transfer) {
            auto& session = *job.session;
            if (code == 0 && session.active && !session.confirmation_pending) {
                session.drain_origin = nullptr;
                session.drain_command.reset();
                metrics_.counter_add("scene_drain_transfers_total");
                common::SceneUnavailable notice;
                notice.set_shard_id(
                    session.record->confirmed_location->shard_id);
                notice.set_binding_epoch(session.record->binding_epoch);
                notice.set_retry_after_seconds(1);
                output(job.session, common::encode(notice));
            } else if (
                code != common::scene_error_outcome_unknown &&
                !session.unresolved && !session.confirmation_pending &&
                session.record && session.record->owner &&
                session.drain_origin &&
                session.record->owner->scene_instance_id ==
                    session.drain_origin->peer->target.instance_id) {
                session.drain_origin = nullptr;
                session.drain_command.reset();
            }
            session.next_drain = Clock::now() + std::chrono::milliseconds(100);
            --job.budget->in_flight;
            if (code != 0)
                metrics_.counter_add(
                    "scene_drain_retries_total",
                    1,
                    {{"reason",
                      code == common::edge_error_throttled ? "capacity"
                                                           : "unavailable"}});
        } else {
            if (code != 0)
                output(
                    job.session,
                    error(job.request_id, code),
                    false,
                    job.control);
            output(job.session, {}, true, job.control, job.budget);
        }
        job.control->stage = Stage::Finished;
        const auto operation =
            !job.command                                           ? "state"
            : job.command->type == common::SceneCommandType::Enter ? "enter"
            : job.command->type == common::SceneCommandType::Transfer
                ? "transfer"
                : "move";
        metrics_.counter_add(
            "scene_commands_total",
            1,
            {{"operation", operation},
             {"outcome",
              code == 0                                     ? "success"
              : code == common::scene_error_outcome_unknown ? "unknown"
                                                            : "rejected"}});
        metrics_.histogram_observe(
            "scene_command_duration_seconds",
            std::chrono::duration<double>(Clock::now() - job.submitted).count(),
            {{"operation", operation}});
        job.session->busy = false;
    }
    Task<> retire(std::shared_ptr<Session> session) {
        const auto old = session->retirements.begin()->second;
        const auto shard = old.confirmed_location->shard_id;
        bool finished = false;
        try {
            if (dynamic_) {
                const auto& current =
                    ownership_[static_cast<std::size_t>(shard - 1)];
                finished =
                    current && (current->version != old.owner->shard_version ||
                                !current->owner ||
                                current->owner->instance_id !=
                                    old.owner->scene_instance_id ||
                                current->owner->incarnation !=
                                    old.owner->scene_incarnation);
            }
            auto& slot = peer(old);
            if (!finished &&
                slot.peer->incarnation() == old.owner->scene_incarnation)
                static_cast<void>(co_await exchange(
                    slot,
                    routed(internal::OPERATION_REMOVE, old),
                    Clock::now() + scene_operation_timeout));
            finished = true;
        } catch (...) {
            // 保持同一已认证连接时重试拥塞/暂时拒绝；连接已失效则旧成员随连接回收。
            try {
                finished = peer(old).peer->incarnation() !=
                           old.owner->scene_incarnation;
            } catch (...) {
                finished = true;
            }
        }
        const auto found = session->retirements.find(shard);
        if (finished && found != session->retirements.end() &&
            found->second.binding_epoch == old.binding_epoch)
            session->retirements.erase(found);
        session->retire_after = Clock::now() + std::chrono::milliseconds(100);
        session->retiring = false;
    }
    Task<> cleanup(std::shared_ptr<Session> session) {
        try {
            if (session->unresolved) {
                const auto write = *session->unresolved;
                auto pending = database([this, write] {
                    return store_.commit_scene(write);
                });
                const auto value = co_await pending;
                if (value.state) session->record = value.state;
                if (value.outcome == common::SceneWriteOutcome::Committed ||
                    value.outcome == common::SceneWriteOutcome::Replayed) {
                    committed(*session, *value.state);
                } else if (
                    value.outcome == common::SceneWriteOutcome::StaleBinding)
                    session->unresolved.reset();
                else {
                    if (stopping_) session->reclaimed = true;
                    session->cleaning = false;
                    co_return;
                }
            }
            if (owned(*session)) {
                const auto old = *session->record;
                auto pending = database([this, old] {
                    return store_.revoke_scene(
                        old.character, old.binding_epoch, *old.owner);
                });
                static_cast<void>(co_await pending);
                session->retirements.insert_or_assign(
                    old.confirmed_location->shard_id, old);
            } else if (session->last_owned) {
                session->retirements.insert_or_assign(
                    session->last_owned->confirmed_location->shard_id,
                    *session->last_owned);
            }
            session->reclaimed = true;
        } catch (...) {
        }
        // 运行时保留重试；停机时已发出的写均已结算，只尝试一次清理。
        // 数据源故障时保留旧持久事实，由新 Realm incarnation 重新绑定。
        if (stopping_) session->reclaimed = true;
        session->cleaning = false;
    }
    void push(const common::ScenePush& value) {
        const auto shard = value.event().shard_id();
        auto* found = instance(value.scene_instance_id());
        if (!found || !targets_.contains(shard) ||
            value.realm_incarnation() != incarnation_ ||
            value.scene_incarnation() != found->peer->incarnation())
            return;
        const auto& event = value.event();
        for (const auto& [binding, session] : tracked_) {
            static_cast<void>(binding);
            if (!session->alive || !session->active ||
                session->snapshot_required || session->resync ||
                !owned(*session) ||
                session->record->owner->scene_instance_id !=
                    value.scene_instance_id() ||
                session->record->owner->scene_incarnation !=
                    value.scene_incarnation() ||
                session->record->confirmed_location->shard_id != shard)
                continue;
            if (session->snapshot.shard_incarnation() !=
                    event.shard_incarnation() ||
                event.event_seq() > session->snapshot.event_seq() + 1) {
                session->resync = true;
                metrics_.counter_add("scene_snapshot_resync_total");
                continue;
            }
            if (event.event_seq() <= session->snapshot.event_seq()) continue;
            session->snapshot.set_event_seq(event.event_seq());
            bool stale = false;
            for (const auto& [other_binding, other] : tracked_) {
                static_cast<void>(other_binding);
                if (other->character.character_id ==
                        event.member().character_id() &&
                    other->record &&
                    other->record->binding_epoch >
                        event.member().binding_epoch())
                    stale = true;
            }
            if (stale && event.kind() == common::SceneEvent::KIND_LEAVE) {
                stale = false;
                for (const auto& member : session->snapshot.members())
                    if (member.character_id() ==
                            event.member().character_id() &&
                        member.binding_epoch() > event.member().binding_epoch())
                        stale = true;
            }
            if (stale) {
                metrics_.counter_add("scene_stale_rejections_total");
                continue;
            }
            auto* members = session->snapshot.mutable_members();
            for (int i = 0; i < members->size(); ++i)
                if (members->Get(i).character_id() ==
                    event.member().character_id()) {
                    members->DeleteSubrange(i, 1);
                    break;
                }
            if (event.kind() != common::SceneEvent::KIND_LEAVE)
                *session->snapshot.add_members() = event.member();
            output(session, common::encode(event));
        }
    }
    Task<> health(PeerSlot& slot) {
        try {
            auto probe = request(
                slot,
                dynamic_ ? internal::OPERATION_STATUS
                         : internal::OPERATION_SNAPSHOT);
            if (!dynamic_) probe.set_shard_id(slot.peer->target.shard_id);
            const auto deadline = Clock::now() + scene_operation_timeout;
            const auto reply =
                co_await exchange(slot, std::move(probe), deadline);
            if (dynamic_) {
                slot.ready = reply.dynamic() && reply.business_ready();
                slot.draining = reply.draining();
                slot.drain_failed =
                    reply.drain_phase() == internal::DRAIN_PHASE_FAILED;
                slot.hosted.clear();
                for (const auto& shard : reply.shards())
                    slot.hosted[shard.shard_id()] = shard;
                std::vector<std::shared_ptr<Session>> affected;
                for (const auto& [binding, session] : tracked_) {
                    static_cast<void>(binding);
                    if (session->active && session->resync && !session->busy &&
                        owned(*session) &&
                        session->record->owner->scene_instance_id ==
                            slot.peer->target.instance_id)
                        affected.push_back(session);
                }
                for (const auto& session : affected) {
                    if (session->busy || !session->alive || !session->active)
                        continue;
                    session->busy = true;
                    try {
                        co_await snapshot(*session, deadline);
                        session->snapshot_required = true;
                        common::SceneUnavailable notice;
                        notice.set_shard_id(
                            session->record->confirmed_location->shard_id);
                        notice.set_binding_epoch(
                            session->record->binding_epoch);
                        notice.set_retry_after_seconds(1);
                        output(session, common::encode(notice));
                    } catch (...) {
                        unavailable(session);
                    }
                    session->busy = false;
                }
            } else {
                for (const auto& [binding, session] : tracked_) {
                    static_cast<void>(binding);
                    if (session->active && session->resync && owned(*session) &&
                        session->record->owner->scene_instance_id ==
                            slot.peer->target.instance_id &&
                        session->record->owner->scene_incarnation ==
                            reply.scene_incarnation()) {
                        session->snapshot = reply.snapshot();
                        session->resync = false;
                        session->snapshot_required = true;
                        common::SceneUnavailable notice;
                        notice.set_shard_id(slot.peer->target.shard_id);
                        notice.set_binding_epoch(
                            session->record->binding_epoch);
                        notice.set_retry_after_seconds(1);
                        output(session, common::encode(notice));
                    }
                }
            }
        } catch (...) {
            if (dynamic_) slot.ready = false;
        }
        slot.health_busy = false;
        slot.next_health =
            Clock::now() + (dynamic_ ? std::chrono::milliseconds(100)
                                     : std::chrono::milliseconds(1000));
    }
    void remember(const common::SceneShardRecord& record) {
        const auto index = static_cast<std::size_t>(record.shard_id - 1);
        if (ownership_[index] &&
            (ownership_[index]->version > record.version ||
             (ownership_[index]->version == record.version &&
              ownership_[index]->draining && !record.draining)))
            return;
        ownership_[index] = record;
        if (record.owner) {
            for (const auto& [id, slot] : peers_)
                if (slot->peer->target.instance_id == record.owner->instance_id)
                    routes_[index] = id;
        }
        for (const auto& [binding, session] : tracked_) {
            static_cast<void>(binding);
            if (!session->active || !session->record ||
                !session->record->owner ||
                session->record->confirmed_location->shard_id !=
                    record.shard_id)
                continue;
            const auto& old = *session->record->owner;
            if (!record.owner || record.version != old.shard_version ||
                record.owner->instance_id != old.scene_instance_id ||
                record.owner->incarnation != old.scene_incarnation)
                unavailable(session);
        }
    }
    bool pending_on(const PeerSlot& slot) const {
        if (slot.in_flight.load() != 0 || slot.health_busy) return true;
        for (const auto& [binding, session] : tracked_) {
            static_cast<void>(binding);
            if (session->drain_origin == &slot) return true;
            if (session->alive && owned(*session) &&
                session->record->owner->scene_instance_id ==
                    slot.peer->target.instance_id)
                return true;
            for (const auto& [shard, old] : session->retirements) {
                static_cast<void>(shard);
                if (old.owner->scene_instance_id ==
                    slot.peer->target.instance_id)
                    return true;
            }
            if (session->unresolved &&
                (session->unresolved->owner.scene_instance_id ==
                     slot.peer->target.instance_id ||
                 (session->unresolved->expected_owner &&
                  session->unresolved->expected_owner->scene_instance_id ==
                      slot.peer->target.instance_id)))
                return true;
        }
        return false;
    }
    bool candidate(const PeerSlot& slot) const {
        return slot.peer->available() && slot.ready && !slot.draining &&
               !slot.peer->incarnation().empty();
    }
    Task<> topology() {
        try {
            for (const auto& [shard, target] : targets_) {
                static_cast<void>(target);
                auto pending = database([this, shard] {
                    return shard_store_->shard_state(1, shard);
                });
                auto current = co_await pending;
                if (stopping_ || !discovery_ready_) break;
                if (current) remember(*current);
                PeerSlot* old = current && current->owner
                                    ? instance(current->owner->instance_id)
                                    : nullptr;
                const bool failed =
                    old &&
                    (!old->peer->available() ||
                     (!old->peer->incarnation().empty() &&
                      old->peer->incarnation() != current->owner->incarnation));
                if (current && current->owner && !failed) {
                    if (old && current->draining && !old->drain_failed &&
                        !pending_on(*old)) {
                        const auto reported = old->hosted.find(shard);
                        if (reported != old->hosted.end() &&
                            reported->second.version() == current->version &&
                            reported->second.active_members() == 0 &&
                            reported->second.prepared_members() == 0) {
                            auto desired = *current;
                            desired.owner.reset();
                            desired.draining = false;
                            if (desired.version == common::scene_counter_max)
                                continue;
                            ++desired.version;
                            auto saved = database([this, current, desired] {
                                return shard_store_->compare_exchange_shard(
                                    current, desired);
                            });
                            if (co_await saved) {
                                remember(desired);
                                demand_[static_cast<std::size_t>(shard - 1)] =
                                    false;
                                metrics_.counter_add(
                                    "scene_shard_assignments_total",
                                    1,
                                    {{"operation", "release"}});
                            }
                        }
                    }
                    continue;
                }
                if (current && !current->owner &&
                    !demand_[static_cast<std::size_t>(shard - 1)])
                    continue;
                PeerSlot* selected = nullptr;
                // 首次分配等待配置偏好实例真实加入；恢复才在其他就绪实例中选择。
                if (!current) {
                    auto& preferred = *peers_.at(shard);
                    if (candidate(preferred)) selected = &preferred;
                } else {
                    for (auto& [id, slot] : peers_) {
                        static_cast<void>(id);
                        if (candidate(*slot)) {
                            selected = slot.get();
                            break;
                        }
                    }
                }
                if (!selected) continue;
                const auto version = current ? current->version : 0;
                if (version == common::scene_counter_max) continue;
                common::SceneShardRecord desired{
                    1,
                    shard,
                    version + 1,
                    common::SceneShardOwner{
                        selected->peer->target.instance_id,
                        selected->peer->incarnation()},
                    false};
                auto saved = database([this, current, desired] {
                    return shard_store_->compare_exchange_shard(
                        current, desired);
                });
                if (co_await saved) {
                    remember(desired);
                    demand_[static_cast<std::size_t>(shard - 1)] = false;
                    metrics_.counter_add(
                        "scene_shard_assignments_total",
                        1,
                        {{"operation", failed ? "recover" : "assign"}});
                }
            }
        } catch (...) {
            // CAS 响应不确定时不猜测，下一轮 majority 回读同一记录。
            metrics_.counter_add("scene_shard_reconcile_errors_total");
        }
        topology_busy_ = false;
        next_topology_ = Clock::now() + std::chrono::milliseconds(100);
    }
    bool drain(const std::shared_ptr<Session>& session) {
        if (!session->alive || session->busy || session->cleaning ||
            !session->waiting.empty() || !owned(*session) ||
            Clock::now() < session->next_drain)
            return false;
        if (session->drain_origin && session->drain_command &&
            session->active && !session->unresolved &&
            !session->confirmation_pending &&
            session->record->confirmed_location->shard_id ==
                session->drain_command->target_shard_id &&
            session->record->last_result->command.seq >=
                session->drain_command->seq) {
            session->drain_origin = nullptr;
            session->drain_command.reset();
            metrics_.counter_add("scene_drain_transfers_total");
            return false;
        }
        auto* origin = session->drain_origin;
        if (!origin) {
            if (session->unresolved || session->confirmation_pending)
                return false;
            origin = instance(session->record->owner->scene_instance_id);
            if (!origin || !origin->draining || origin->drain_failed)
                return false;
        }
        std::optional<common::SceneCommand> command = session->drain_command;
        if (!command) {
            for (const auto& [shard, target] : targets_) {
                static_cast<void>(target);
                const auto& record =
                    ownership_[static_cast<std::size_t>(shard - 1)];
                if (shard == session->record->confirmed_location->shard_id ||
                    !record || !record->owner || record->draining)
                    continue;
                auto* destination = instance(record->owner->instance_id);
                if (!destination || destination == origin ||
                    !candidate(*destination))
                    continue;
                const auto seq = session->record->last_result->command.seq;
                if (seq == common::scene_counter_max) return false;
                command = common::SceneCommand{
                    common::SceneCommandType::Transfer, seq + 1, 0, 0, shard};
                break;
            }
        }
        if (!command) return false;
        PeerSlot* destination = nullptr;
        if (session->unresolved)
            destination =
                instance(session->unresolved->owner.scene_instance_id);
        else if (
            session->drain_command &&
            session->record->last_result->command == *command)
            destination = instance(session->record->owner->scene_instance_id);
        else {
            const auto& record = ownership_[static_cast<std::size_t>(
                command->target_shard_id - 1)];
            if (record && record->owner)
                destination = instance(record->owner->instance_id);
        }
        if (!destination || !reserve(*destination)) return false;
        auto control = std::make_shared<Control>();
        const auto now = Clock::now();
        control->deadline = now + scene_operation_timeout;
        session->drain_origin = origin;
        session->drain_command = command;
        session->busy = true;
        scheduler_.spawn(process(
            Job{session,
                0,
                control,
                command,
                now,
                control->deadline,
                destination,
                true}));
        return true;
    }
    void run(std::stop_token stop) noexcept {
        while (true) {
            stopping_ = stop.stop_requested();
            for (auto& value : pushes_.drain(256)) push(value);
            scheduler_.poll();
            if (database_) database_->drain();
            for (auto& job : jobs_.drain(128)) {
                auto session = job.session;
                if (!tracked_.contains(session->binding) &&
                    tracked_.size() >= peers_.size() * 64) {
                    output(
                        session,
                        error(job.request_id, common::edge_error_throttled),
                        false,
                        job.control);
                    output(session, {}, true, job.control, job.budget);
                    continue;
                }
                tracked_[session->binding] = session;
                session->waiting.push_back(std::move(job));
            }
            for (auto& [shard, slot] : peers_) {
                static_cast<void>(shard);
                if (slot->generation != slot->peer->generation()) {
                    slot->generation = slot->peer->generation();
                    for (auto& [binding, session] : tracked_) {
                        static_cast<void>(binding);
                        if (session->record && session->record->owner &&
                            session->record->owner->scene_instance_id ==
                                slot->peer->target.instance_id &&
                            session->peer_generation < slot->generation)
                            unavailable(session);
                    }
                }
                if (!stop.stop_requested() && !slot->health_busy &&
                    (dynamic_ ? slot->peer->available()
                              : !slot->peer->incarnation().empty()) &&
                    Clock::now() >= slot->next_health) {
                    slot->health_busy = true;
                    scheduler_.spawn(health(*slot));
                }
            }
            if (dynamic_ && !stopping_ && discovery_ready_ && !topology_busy_ &&
                Clock::now() >= next_topology_) {
                topology_busy_ = true;
                scheduler_.spawn(topology());
            }
            std::map<std::uint64_t, Session*> character_busy;
            for (const auto& [binding, session] : tracked_) {
                static_cast<void>(binding);
                if (session->busy || session->unresolved)
                    character_busy[session->character.character_id] =
                        session.get();
            }
            for (auto& [binding, session] : tracked_) {
                static_cast<void>(binding);
                const auto owner =
                    character_busy.find(session->character.character_id);
                if (!session->busy && !session->waiting.empty() &&
                    (owner == character_busy.end() ||
                     owner->second == session.get())) {
                    character_busy[session->character.character_id] =
                        session.get();
                    session->busy = true;
                    auto job = std::move(session->waiting.front());
                    session->waiting.pop_front();
                    scheduler_.spawn(process(std::move(job)));
                }
                if (dynamic_ && !stopping_ &&
                    (owner == character_busy.end() ||
                     owner->second == session.get()) &&
                    drain(session))
                    character_busy[session->character.character_id] =
                        session.get();
                if (!stopping_ && !session->retiring &&
                    !session->retirements.empty() &&
                    Clock::now() >= session->retire_after) {
                    session->retiring = true;
                    scheduler_.spawn(retire(session));
                }
                if (!session->alive && !session->busy &&
                    session->waiting.empty() && !session->cleaning &&
                    !session->reclaimed) {
                    session->cleaning = true;
                    scheduler_.spawn(cleanup(session));
                }
                // 不再向故障实例续发 REMOVE；等待在途调用结束后，由 Peer
                // 析构关闭连接，Scene 按连接失效回收可见成员。
                if (stopping_ && !session->retiring)
                    session->retirements.clear();
            }
            std::erase_if(tracked_, [](const auto& item) {
                const auto& s = *item.second;
                return s.reclaimed && !s.retiring && s.retirements.empty();
            });
            std::size_t active = 0, unresolved = 0, drain_pending = 0;
            for (const auto& [binding, session] : tracked_) {
                static_cast<void>(binding);
                if (session->alive && session->active) ++active;
                if (session->unresolved) ++unresolved;
                if (session->drain_origin) ++drain_pending;
            }
            metrics_.gauge_set(
                "scene_drain_pending_transfers",
                static_cast<double>(drain_pending));
            std::size_t ready_instances = 0;
            for (const auto& [id, slot] : peers_) {
                static_cast<void>(id);
                if (dynamic_ && candidate(*slot)) ++ready_instances;
            }
            metrics_.gauge_set(
                "scene_ready_instances", static_cast<double>(ready_instances));
            metrics_.gauge_set(
                "scene_active_bindings", static_cast<double>(active));
            metrics_.gauge_set(
                "scene_pending_readbacks", static_cast<double>(unresolved));
            if (stop.stop_requested() && tracked_.empty() && scheduler_.idle())
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    common::RealmSceneStore& store_;
    RealmOutbox& outbox_;
    observability::MetricsRegistry& metrics_;
    common::SceneShardStore* shard_store_;
    bool dynamic_;
    std::string incarnation_;
    std::map<std::uint64_t, std::unique_ptr<PeerSlot>> peers_;
    std::map<std::uint64_t, RealmConfig::SceneTarget> targets_;
    std::array<std::atomic_uint64_t, 2> routes_{1, 2};
    std::array<std::atomic_bool, 2> demand_{false, false};
    std::array<std::optional<common::SceneShardRecord>, 2> ownership_;
    std::atomic_bool discovery_ready_{false};
    bool topology_busy_{false};
    Clock::time_point next_topology_{Clock::now()};
    std::unordered_map<edge::EdgeConnectionId, std::shared_ptr<Session>>
        sessions_;
    std::uint64_t next_binding_{1};
    concurrency::BoundedQueue<Job> jobs_{128};
    concurrency::BoundedQueue<Output> outputs_{1024};
    concurrency::BoundedQueue<common::ScenePush> pushes_{1024};
    std::map<std::uint64_t, std::shared_ptr<Session>> tracked_;
    scene_detail::Scheduler scheduler_;
    std::unique_ptr<scene_detail::DatabaseJobs> database_;
    bool stopping_{false};  // 仅编排线程访问。
    std::jthread thread_;
};
RealmScenes::RealmScenes(
    common::RealmSceneStore& store,
    RealmOutbox& outbox,
    const RealmConfig& config,
    observability::MetricsRegistry& metrics,
    common::SceneShardStore* shards)
    : worker_(
          std::make_unique<Worker>(store, outbox, config, metrics, shards)) {}
RealmScenes::~RealmScenes() = default;
void RealmScenes::selected(
    edge::EdgeConnectionId id, common::SceneCharacterKey character) {
    worker_->selected(id, character);
}
void RealmScenes::closed(edge::EdgeConnectionId id) { worker_->closed(id); }
bool RealmScenes::handle(
    edge::EdgeConnectionId id, std::span<const std::byte> bytes) {
    return worker_->handle(id, bytes);
}
void RealmScenes::complete(std::size_t max) { worker_->complete(max); }
std::optional<RealmScenes::ChatBinding> RealmScenes::chat_binding(
    edge::EdgeConnectionId id) const {
    return worker_->chat_binding(id);
}
void RealmScenes::endpoint(
    const std::optional<network::TransportEndpoint>& value,
    std::string_view instance_id) {
    worker_->endpoint(value, instance_id);
}
void RealmScenes::discovery_ready(bool ready) {
    worker_->discovery_ready(ready);
}
}  // namespace realm::game::realm
