#include "realmmesh/game/realm/realm_scenes.hpp"

#include "realmmesh/concurrency/bounded_queue.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/scene_internal_protocol.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/network/client/tls_client_stream.hpp"
#include "realmmesh/network/codec/length_field_codec.hpp"
#include "realmmesh/observability/metrics_registry.hpp"

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
struct Failure {
    int code;
};
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
public:
    enum class Stage { Queued, Reading, Writing, Finished, Cancelled };
    struct Control {
        std::uint64_t request_id{0};
        Clock::time_point deadline;
        std::atomic<Stage> stage{Stage::Queued};
        std::atomic_bool timed_out{false};
    };
    struct Session {
        edge::EdgeConnectionId id;
        common::SceneCharacterKey character;
        std::uint64_t binding{0};
        std::atomic_bool alive{true};
        std::size_t outstanding{0};  // 帧线程专属。
        std::vector<std::shared_ptr<Control>> pending;
        // 下列字段仅场景工作线程访问。
        std::optional<common::SceneStateRecord> record;
        std::optional<common::SceneWrite> unresolved;
        bool active{false};
        bool confirmation_pending{false};
        bool resync{false};
        bool snapshot_required{false};
        wire::Snapshot snapshot;
    };
    struct Job {
        std::shared_ptr<Session> session;
        std::uint64_t request_id;
        std::shared_ptr<Control> control;
        std::optional<common::SceneCommand> command;
        Clock::time_point submitted;
        Clock::time_point deadline;
    };
    struct Output {
        std::shared_ptr<Session> session;
        std::vector<std::byte> payload;
        bool done{false};
        std::shared_ptr<Control> control;
    };
    Worker(
        common::RealmSceneStore& store,
        RealmOutbox& outbox,
        const RealmConfig& config,
        observability::MetricsRegistry& metrics)
        : store_(store),
          outbox_(outbox),
          config_(config.scene),
          metrics_(metrics),
          incarnation_(
              common::correlation_id_hex(common::make_correlation_id())),
          thread_([this](std::stop_token stop) {
              run(stop);
          }) {}
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
        if (found == sessions_.end()) return;
        found->second->alive = false;
        sessions_.erase(found);
    }
    bool handle(edge::EdgeConnectionId id, std::span<const std::byte> payload) {
        const auto found = sessions_.find(id);
        if (found == sessions_.end()) return false;
        const auto message = common::scene_message_id(payload);
        if (!message) return false;
        std::optional<common::SceneCommand> command;
        if (const auto enter = common::decode_enter_scene(payload)) {
            command = common::SceneCommand{
                common::SceneCommandType::Enter, enter->scene_seq()};
        } else if (const auto move = common::decode_move_in_scene(payload)) {
            command = common::SceneCommand{
                common::SceneCommandType::Move,
                move->scene_seq(),
                move->x(),
                move->y()};
        } else if (!common::decode_get_scene_state(payload))
            return false;
        const auto request_id = common::scene_request_id(payload).value_or(0);
        auto session = found->second;
        if (session->outstanding >= 9 || in_flight_.load() >= 64) {
            outbox_.send(id, error(request_id, common::edge_error_throttled));
            metrics_.counter_add("scene_queue_rejections_total");
            return true;
        }
        const auto now = Clock::now();
        auto control = std::make_shared<Control>();
        control->request_id = request_id;
        control->deadline = now + scene_operation_timeout;
        ++in_flight_;
        if (!jobs_.try_push(
                Job{session,
                    request_id,
                    control,
                    command,
                    now,
                    control->deadline})) {
            --in_flight_;
            outbox_.send(id, error(request_id, common::edge_error_throttled));
            metrics_.counter_add("scene_queue_rejections_total");
            return true;
        }
        ++session->outstanding;
        session->pending.push_back(std::move(control));
        return true;
    }
    void complete(std::size_t max) {
        for (auto& output : outputs_.drain(max)) {
            if (output.done) {
                --output.session->outstanding;
                --in_flight_;
                std::erase(output.session->pending, output.control);
            }
            const auto found = sessions_.find(output.session->id);
            if (found != sessions_.end() && found->second == output.session &&
                output.session->alive && !output.payload.empty() &&
                (!output.control || !output.control->timed_out)) {
                outbox_.send(output.session->id, std::move(output.payload));
            }
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
                // 读/排队可取消;已发出的写只报告不确定,继续后台对账。
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
    void endpoint(const std::optional<network::TransportEndpoint>& value) {
        const std::scoped_lock lock(endpoint_mutex_);
        endpoint_ = value;
    }

private:
    static std::vector<std::byte> error(std::uint64_t id, int code) {
        common::EdgeError error;
        error.set_code(static_cast<std::uint32_t>(code));
        error.set_message("scene request unavailable or rejected");
        if (code == common::edge_error_throttled)
            error.set_retry_after_seconds(1);
        return common::encode(error, id);
    }
    void output(
        const std::shared_ptr<Session>& session,
        std::vector<std::byte> payload,
        bool done = false,
        std::shared_ptr<Control> control = {}) {
        if (!outputs_.try_push(
                Output{
                    session, std::move(payload), done, std::move(control)})) {
            // 帧线程长期不消费时只关闭该观察者;绝不阻塞整个分片。
            session->alive = false;
            if (done) --in_flight_;
        }
    }
    bool owned(const Session& session) const {
        return session.record && session.record->owner &&
               session.record->owner->realm_incarnation == incarnation_ &&
               session.record->owner->session_binding == session.binding;
    }
    void unavailable(const std::shared_ptr<Session>& session) {
        if (!session->active) return;
        session->active = false;
        common::SceneUnavailable notice;
        notice.set_shard_id(1);
        notice.set_binding_epoch(session->record->binding_epoch);
        notice.set_retry_after_seconds(1);
        output(session, common::encode(notice));
    }
    void disconnect() {
        if (stream_) stream_->shutdown();
        stream_.reset();
        input_.clear();
        scene_incarnation_.clear();
        for (auto& [binding, session] : tracked_) {
            static_cast<void>(binding);
            unavailable(session);
        }
    }
    std::optional<std::vector<std::byte>> receive(Clock::time_point deadline) {
        while (stream_) {
            auto decoded = codec_.try_decode(input_);
            if (decoded.status == network::DecodeStatus::FrameReady)
                return std::move(decoded.payload);
            if (decoded.status == network::DecodeStatus::FrameTooLarge) {
                disconnect();
                return std::nullopt;
            }
            std::array<std::byte, 8192> chunk{};
            const auto read = stream_->read_some(chunk, deadline);
            if (!read) return std::nullopt;
            if (*read == 0) {
                disconnect();
                return std::nullopt;
            }
            input_.append(std::span<const std::byte>(chunk.data(), *read));
        }
        return std::nullopt;
    }
    common::SceneRequest request(internal::Operation operation) const {
        common::SceneRequest request;
        request.set_operation(operation);
        request.set_realm_incarnation(incarnation_);
        request.set_scene_instance_id(config_.instance_id);
        request.set_scene_incarnation(
            scene_incarnation_.empty() ? "discover" : scene_incarnation_);
        return request;
    }
    common::SceneRequest routed(
        internal::Operation operation,
        const common::SceneStateRecord& record) const {
        auto message = request(operation);
        auto* route = message.mutable_route();
        route->set_realm_id(record.character.realm_id);
        route->set_character_id(record.character.character_id);
        route->set_binding_epoch(record.binding_epoch);
        route->set_session_binding(record.owner->session_binding);
        route->set_shard_id(record.confirmed_location->shard_id);
        return message;
    }
    void push(const common::ScenePush& push) {
        if (push.realm_incarnation() != incarnation_ ||
            push.scene_instance_id() != config_.instance_id ||
            push.scene_incarnation() != scene_incarnation_)
            return;
        const auto& event = push.event();
        for (const auto& [binding, session] : tracked_) {
            static_cast<void>(binding);
            if (!session->alive || !session->active ||
                session->snapshot_required || session->resync)
                continue;
            if (!owned(*session) ||
                session->record->owner->scene_incarnation != scene_incarnation_)
                continue;
            if (event.shard_incarnation() !=
                    session->snapshot.shard_incarnation() ||
                event.event_seq() > session->snapshot.event_seq() + 1) {
                session->resync = true;
                metrics_.counter_add("scene_snapshot_resync_total");
                continue;
            }
            if (event.event_seq() <= session->snapshot.event_seq()) continue;
            // 针对本进程已知角色,旧绑定的事件只推进水位,不发布旧位置。
            bool stale = false;
            for (const auto& [other_binding, other] : tracked_) {
                static_cast<void>(other_binding);
                if (other->character.character_id ==
                        event.member().character_id() &&
                    other->record &&
                    event.member().binding_epoch() <
                        other->record->binding_epoch)
                    stale = true;
            }
            session->snapshot.set_event_seq(event.event_seq());
            if (stale) {
                metrics_.counter_add("scene_stale_rejections_total");
                continue;
            }
            auto* members = session->snapshot.mutable_members();
            for (int i = 0; i < members->size(); ++i) {
                if (members->Get(i).character_id() ==
                    event.member().character_id()) {
                    members->DeleteSubrange(i, 1);
                    break;
                }
            }
            if (event.kind() != common::SceneEvent::KIND_LEAVE)
                *session->snapshot.add_members() = event.member();
            output(session, common::encode(event));
        }
    }
    common::SceneReply exchange(
        common::SceneRequest message, Clock::time_point deadline) {
        if (!stream_ || Clock::now() >= deadline)
            throw Failure{common::scene_error_unavailable};
        const auto id = next_request_id_++;
        if (!stream_->write_all(
                codec_.encode(common::encode(message, id)), deadline)) {
            disconnect();
            throw Failure{common::scene_error_unavailable};
        }
        while (Clock::now() < deadline) {
            const auto payload = receive(deadline);
            if (!payload) break;
            if (const auto event = common::decode_scene_push(*payload)) {
                push(*event);
                continue;
            }
            const auto reply = common::decode_scene_reply(*payload);
            if (!reply || common::scene_internal_request_id(*payload) != id ||
                reply->operation() != message.operation() ||
                reply->realm_incarnation() != incarnation_ ||
                reply->scene_instance_id() != config_.instance_id ||
                (!scene_incarnation_.empty() &&
                 reply->scene_incarnation() != scene_incarnation_))
                break;
            if (reply->status() != internal::STATUS_OK) {
                if (reply->status() == internal::STATUS_BUSY)
                    throw Failure{common::edge_error_throttled};
                if (reply->status() == internal::STATUS_INVALID)
                    throw Failure{common::scene_error_invalid_target};
                throw Failure{common::scene_error_stale_binding};
            }
            if (message.has_route() && reply->route().SerializeAsString() !=
                                           message.route().SerializeAsString())
                break;
            if (message.scene_seq() != reply->scene_seq()) break;
            return *reply;
        }
        disconnect();
        throw Failure{common::scene_error_unavailable};
    }
    void connect(Clock::time_point deadline) {
        if (stream_) return;
        if (!config_.enabled) throw Failure{common::scene_error_unavailable};
        auto host = config_.host;
        auto port = config_.port;
        {
            const std::scoped_lock lock(endpoint_mutex_);
            if (endpoint_) {
                host = endpoint_->address;
                port = endpoint_->port;
            }
        }
        network::client::TlsClientOptions tls;
        tls.alpn = common::scene_internal_alpn;
        tls.ca_file = config_.ca_file;
        tls.certificate_chain_file = config_.certificate_file;
        tls.private_key_file = config_.private_key_file;
        tls.server_identity = config_.server_identity;
        const auto dial =
            network::client::TlsClientStream::dial(host, port, tls, deadline);
        if (!dial.ok() ||
            dial.stream->negotiated_alpn() != common::scene_internal_alpn)
            throw Failure{common::scene_error_unavailable};
        stream_ = dial.stream;
        auto bootstrap = request(internal::OPERATION_SNAPSHOT);
        bootstrap.set_shard_id(1);
        const auto reply = exchange(std::move(bootstrap), deadline);
        scene_incarnation_ = reply.scene_incarnation();
    }
    common::SceneState state(const Session& session) const {
        common::SceneState value;
        value.set_phase(
            session.active   ? wire::PHASE_ACTIVE
            : owned(session) ? wire::PHASE_UNAVAILABLE
                             : wire::PHASE_OUTSIDE);
        if (session.record) {
            value.set_binding_epoch(session.record->binding_epoch);
            if (session.record->last_result) {
                value.set_last_scene_seq(
                    session.record->last_result->command.seq);
                *value.mutable_last_result() =
                    result(*session.record->last_result, true);
                *value.mutable_confirmed_location() =
                    location(*session.record->confirmed_location);
            }
        }
        if (session.active) *value.mutable_snapshot() = session.snapshot;
        return value;
    }
    void snapshot(Session& session, Clock::time_point deadline) {
        auto message = request(internal::OPERATION_SNAPSHOT);
        message.set_shard_id(1);
        const auto reply = exchange(std::move(message), deadline);
        bool contains = false;
        for (const auto& member : reply.snapshot().members()) {
            if (member.character_id() == session.character.character_id &&
                member.binding_epoch() == session.record->binding_epoch)
                contains = true;
        }
        if (!contains) {
            session.active = false;
            throw Failure{common::scene_error_stale_binding};
        }
        session.snapshot = reply.snapshot();
        session.resync = false;
    }
    void restore(Session& session, Clock::time_point deadline) {
        if (!owned(session) ||
            session.record->owner->scene_incarnation != scene_incarnation_)
            throw Failure{common::scene_error_stale_binding};
        const auto& receipt = *session.record->last_result;
        auto prepare = routed(internal::OPERATION_PREPARE, *session.record);
        prepare.set_scene_seq(receipt.command.seq);
        prepare.set_command_digest(digest(receipt.command));
        *prepare.mutable_location() = location(receipt.location);
        static_cast<void>(exchange(std::move(prepare), deadline));
        auto activate = routed(internal::OPERATION_ACTIVATE, *session.record);
        activate.set_scene_seq(receipt.command.seq);
        activate.set_command_digest(digest(receipt.command));
        session.snapshot = exchange(std::move(activate), deadline).snapshot();
        session.active = true;
        session.resync = false;
    }
    void execute(const Job& job) {
        auto& session = *job.session;
        if (!session.alive) return;
        Stage queued = Stage::Queued;
        if (!job.control->stage.compare_exchange_strong(queued, Stage::Reading))
            throw Failure{common::scene_error_unavailable};
        if (Clock::now() >= job.deadline)
            throw Failure{common::scene_error_unavailable};
        if (!tracked_.contains(session.binding) && tracked_.size() >= 64)
            throw Failure{common::edge_error_throttled};
        tracked_[session.binding] = job.session;
        session.record = store_.initialize_scene(session.character);
        if (!session.record) throw Failure{common::scene_error_stale_binding};
        if (!owned(session) && session.active) unavailable(job.session);
        if (session.unresolved) {
            if (common::resolve_scene_write(
                    *session.unresolved, session.record) ==
                common::SceneWriteResolution::Committed) {
                session.unresolved.reset();
            } else if (
                !job.command || job.command != session.unresolved->command) {
                throw Failure{common::scene_error_outcome_unknown};
            }
        }
        if (!job.command) {
            if (session.confirmation_pending && owned(session)) {
                try {
                    connect(job.deadline);
                    restore(session, job.deadline);
                    session.confirmation_pending = false;
                } catch (...) {
                    unavailable(job.session);
                }
            }
            if (session.active) {
                try {
                    connect(job.deadline);
                    snapshot(session, job.deadline);
                } catch (const Failure&) {
                    unavailable(job.session);
                }
            }
            session.snapshot_required = false;
            output(
                job.session,
                common::encode(state(session), job.request_id),
                false,
                job.control);
            return;
        }
        const auto command = *job.command;
        if (session.confirmation_pending && session.record->last_result &&
            command != session.record->last_result->command)
            throw Failure{common::scene_error_outcome_unknown};
        const auto previous = session.record->last_result;
        const bool replayed = previous && previous->command == command;
        if (previous && command.seq <= previous->command.seq && !replayed)
            throw Failure{common::scene_error_invalid_seq};
        if (!replayed &&
            command.seq != (previous ? previous->command.seq + 1 : 1))
            throw Failure{common::scene_error_invalid_seq};
        if (replayed) {
            if (!owned(session))
                throw Failure{common::scene_error_stale_binding};
            connect(job.deadline);
            restore(session, job.deadline);
            session.confirmation_pending = false;
            output(
                job.session,
                common::encode(result(*previous, true), job.request_id),
                false,
                job.control);
            return;
        }
        if (command.type == common::SceneCommandType::Move &&
            (!session.active || !owned(session)))
            throw Failure{common::scene_error_not_active};
        connect(job.deadline);
        common::SceneWrite write;
        write.character = session.character;
        write.command = command;
        write.expected_epoch = session.record->binding_epoch;
        write.expected_owner = session.record->owner;
        write.owner = common::SceneOwner{
            incarnation_,
            session.binding,
            config_.instance_id,
            scene_incarnation_};
        write.location = command.type == common::SceneCommandType::Enter
                             ? session.record->confirmed_location.value_or(
                                   common::SceneLocation{1, 0, 0})
                             : common::SceneLocation{1, command.x, command.y};
        if (write.location.shard_id != 1 ||
            session.record->binding_epoch >= common::scene_counter_max)
            throw Failure{common::scene_error_stale_binding};
        auto candidate = *session.record;
        candidate.owner = write.owner;
        candidate.confirmed_location = write.location;
        if (command.type == common::SceneCommandType::Enter)
            ++candidate.binding_epoch;
        if (command.type == common::SceneCommandType::Move &&
            session.record->owner != std::optional(write.owner))
            throw Failure{common::scene_error_stale_binding};
        auto proposal = routed(
            command.type == common::SceneCommandType::Enter
                ? internal::OPERATION_PREPARE
                : internal::OPERATION_PROPOSE_MOVE,
            candidate);
        proposal.set_scene_seq(command.seq);
        proposal.set_command_digest(digest(command));
        *proposal.mutable_location() = location(write.location);
        const auto proposed = exchange(std::move(proposal), job.deadline);
        if (proposed.location().SerializeAsString() !=
            location(write.location).SerializeAsString())
            throw Failure{common::scene_error_stale_binding};
        if (!session.alive || Clock::now() >= job.deadline)
            throw Failure{common::scene_error_unavailable};
        Stage reading = Stage::Reading;
        if (!job.control->stage.compare_exchange_strong(
                reading, Stage::Writing))
            throw Failure{common::scene_error_unavailable};
        common::SceneWriteResult committed;
        try {
            committed = store_.commit_scene(write);
        } catch (const common::SceneWriteUncertain&) {
            session.unresolved = write;
            metrics_.counter_add("scene_write_uncertain_total");
            do {
                session.record = store_.scene_state(session.character);
                if (common::resolve_scene_write(write, session.record) ==
                    common::SceneWriteResolution::Committed) {
                    session.unresolved.reset();
                    committed = {
                        common::SceneWriteOutcome::Committed, session.record};
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            } while (Clock::now() < job.deadline && session.alive);
            if (session.unresolved)
                throw Failure{common::scene_error_outcome_unknown};
        }
        if (committed.outcome != common::SceneWriteOutcome::Committed &&
            committed.outcome != common::SceneWriteOutcome::Replayed) {
            if (committed.outcome == common::SceneWriteOutcome::InvalidSequence)
                throw Failure{common::scene_error_invalid_seq};
            throw Failure{common::scene_error_stale_binding};
        }
        session.record = committed.state;
        session.unresolved.reset();
        session.confirmation_pending = true;
        // 关闭恰好发生在提交中时,不得激活/广播;主循环按刚提交的旧绑定清理。
        if (!session.alive) return;
        try {
            if (command.type == common::SceneCommandType::Enter) {
                auto activate =
                    routed(internal::OPERATION_ACTIVATE, *session.record);
                activate.set_scene_seq(command.seq);
                activate.set_command_digest(digest(command));
                session.snapshot =
                    exchange(std::move(activate), job.deadline).snapshot();
                session.active = true;
            } else {
                auto confirm =
                    routed(internal::OPERATION_CONFIRM, *session.record);
                confirm.set_scene_seq(command.seq);
                confirm.set_command_digest(digest(command));
                *confirm.mutable_location() = location(write.location);
                static_cast<void>(exchange(std::move(confirm), job.deadline));
            }
        } catch (...) {
            unavailable(job.session);
            throw Failure{common::scene_error_outcome_unknown};
        }
        session.confirmation_pending = false;
        output(
            job.session,
            common::encode(
                result(
                    *session.record->last_result,
                    committed.outcome == common::SceneWriteOutcome::Replayed),
                job.request_id),
            false,
            job.control);
    }
    void cleanup() {
        for (auto it = tracked_.begin(); it != tracked_.end();) {
            auto& session = *it->second;
            if (session.alive) {
                ++it;
                continue;
            }
            try {
                if (session.unresolved) {
                    // 同一条件写幂等重试,取得终结事实后再撤销;旧 majority 读不
                    // 足以证明原写不会迟到。重试与旧写只能有一个 CAS 胜出。
                    const auto settled =
                        store_.commit_scene(*session.unresolved);
                    if (settled.state) session.record = settled.state;
                    if (settled.outcome ==
                            common::SceneWriteOutcome::Committed ||
                        settled.outcome ==
                            common::SceneWriteOutcome::Replayed ||
                        settled.outcome ==
                            common::SceneWriteOutcome::StaleBinding) {
                        session.unresolved.reset();
                    } else {
                        ++it;
                        continue;
                    }
                }
                if (owned(session)) {
                    const auto old = *session.record;
                    static_cast<void>(store_.revoke_scene(
                        session.character, old.binding_epoch, *old.owner));
                    if (stream_ &&
                        old.owner->scene_incarnation == scene_incarnation_) {
                        try {
                            static_cast<void>(exchange(
                                routed(internal::OPERATION_REMOVE, old),
                                Clock::now() + scene_operation_timeout));
                        } catch (...) {
                            disconnect();
                        }
                    }
                }
                it = tracked_.erase(it);
            } catch (...) {
                ++it;
            }
        }
    }
    void run(std::stop_token stop) noexcept {
        auto health = Clock::now();
        while (!stop.stop_requested()) {
            cleanup();
            if (auto job = jobs_.try_pop()) {
                int code = 0;
                try {
                    execute(*job);
                } catch (const Failure& failure) {
                    code = failure.code;
                } catch (...) {
                    code = job->session->unresolved
                               ? common::scene_error_outcome_unknown
                               : common::scene_error_unavailable;
                }
                if (code != 0 && job->session->confirmation_pending &&
                    job->command)
                    code = common::scene_error_outcome_unknown;
                if (code == common::scene_error_stale_binding)
                    metrics_.counter_add("scene_stale_rejections_total");
                if (code != 0)
                    output(
                        job->session,
                        error(job->request_id, code),
                        false,
                        job->control);
                output(job->session, {}, true, job->control);
                job->control->stage = Stage::Finished;
                const auto operation =
                    !job->command ? "state"
                    : job->command->type == common::SceneCommandType::Enter
                        ? "enter"
                        : "move";
                metrics_.counter_add(
                    "scene_commands_total",
                    1,
                    {{"operation", operation},
                     {"outcome",
                      code == 0 ? "success"
                      : code == common::scene_error_outcome_unknown
                          ? "unknown"
                          : "rejected"}});
                metrics_.histogram_observe(
                    "scene_command_duration_seconds",
                    std::chrono::duration<double>(Clock::now() - job->submitted)
                        .count(),
                    {{"operation", operation}});
            } else if (stream_) {
                try {
                    const auto bytes =
                        receive(Clock::now() + std::chrono::milliseconds(10));
                    if (bytes) {
                        if (const auto event =
                                common::decode_scene_push(*bytes))
                            push(*event);
                        else
                            disconnect();
                    }
                    if (stream_ && Clock::now() >= health) {
                        auto check = request(internal::OPERATION_SNAPSHOT);
                        check.set_shard_id(1);
                        const auto snapshot =
                            exchange(
                                std::move(check),
                                Clock::now() + scene_operation_timeout)
                                .snapshot();
                        for (auto& [binding, session] : tracked_) {
                            static_cast<void>(binding);
                            if (session->active && session->resync) {
                                session->snapshot = snapshot;
                                session->resync = false;
                                session->snapshot_required = true;
                                common::SceneUnavailable notice;
                                notice.set_shard_id(1);
                                notice.set_binding_epoch(
                                    session->record->binding_epoch);
                                notice.set_retry_after_seconds(1);
                                output(session, common::encode(notice));
                            }
                        }
                        health = Clock::now() + std::chrono::seconds(1);
                    }
                } catch (...) {
                    disconnect();
                }
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            std::size_t active = 0, unresolved = 0;
            for (const auto& [binding, session] : tracked_) {
                static_cast<void>(binding);
                if (session->alive && session->active) ++active;
                if (session->unresolved) ++unresolved;
            }
            metrics_.gauge_set(
                "scene_active_bindings", static_cast<double>(active));
            metrics_.gauge_set(
                "scene_pending_readbacks", static_cast<double>(unresolved));
        }
        cleanup();
        disconnect();
    }
    common::RealmSceneStore& store_;
    RealmOutbox& outbox_;
    RealmConfig::SceneClient config_;
    observability::MetricsRegistry& metrics_;
    std::string incarnation_;
    std::unordered_map<edge::EdgeConnectionId, std::shared_ptr<Session>>
        sessions_;  // 帧线程。
    std::uint64_t next_binding_{1};
    concurrency::BoundedQueue<Job> jobs_{64};
    concurrency::BoundedQueue<Output> outputs_{1024};
    std::atomic_size_t in_flight_{0};
    std::mutex endpoint_mutex_;
    std::optional<network::TransportEndpoint> endpoint_;
    std::map<std::uint64_t, std::shared_ptr<Session>> tracked_;  // 工作线程。
    std::shared_ptr<network::client::TlsClientStream> stream_;
    network::LengthFieldCodec codec_{common::scene_envelope_max};
    network::ByteBuffer input_;
    std::string scene_incarnation_;
    std::uint64_t next_request_id_{1};
    std::jthread thread_;  // 最后声明,成员初始化完毕后才启动。
};

RealmScenes::RealmScenes(
    common::RealmSceneStore& store,
    RealmOutbox& outbox,
    const RealmConfig& config,
    observability::MetricsRegistry& metrics)
    : worker_(std::make_unique<Worker>(store, outbox, config, metrics)) {}
RealmScenes::~RealmScenes() = default;
void RealmScenes::selected(
    edge::EdgeConnectionId id, common::SceneCharacterKey character) {
    worker_->selected(id, character);
}
void RealmScenes::closed(edge::EdgeConnectionId id) { worker_->closed(id); }
bool RealmScenes::handle(
    edge::EdgeConnectionId id, std::span<const std::byte> payload) {
    return worker_->handle(id, payload);
}
void RealmScenes::complete(std::size_t max_completions) {
    worker_->complete(max_completions);
}
void RealmScenes::endpoint(
    const std::optional<network::TransportEndpoint>& value) {
    worker_->endpoint(value);
}
}  // namespace realm::game::realm
