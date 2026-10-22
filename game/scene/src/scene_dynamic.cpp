#include "scene_dynamic.hpp"
#include "realmmesh/concurrency/bounded_work_pool.hpp"
#include "realmmesh/edge/edge_runtime.hpp"
#include "realmmesh/game/common/player_data_config_lua.hpp"
#include "realmmesh/game/common/session_ticket.hpp"
#include "realmmesh/game/scene/scene_shard.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/service_host/edge_service_module.hpp"
#include "realmmesh/service_host/host_config.hpp"
#include "realmmesh/service_host/layered_config_lua.hpp"

#include <array>
#include <atomic>
#include <deque>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>

namespace realm::game::scene {
namespace internal = ::realmmesh::protocol::scene_internal::v1;
namespace {
using Clock = std::chrono::steady_clock;
using Id = edge::EdgeConnectionId;
class DynamicSceneModule final : public service_host::EdgeServiceModule {
    struct Pending {
        Id connection;
        common::SceneRequest request;
        std::uint64_t correlation;
        Clock::time_point deadline;
    };
    struct Peer {
        std::string realm_incarnation;
        bool accepted{false};
        std::uint64_t busy{0};
        std::deque<Pending> waiting;
    };
    struct Checked {
        std::array<bool, 2> observed{};
        std::array<std::optional<common::SceneShardRecord>, 2> records;
        bool failed{false};
        bool instance_checked{false}, drain_fenced{false};
    };

public:
    explicit DynamicSceneModule(
        const service_host::ServiceModuleContext& context)
        : EdgeServiceModule(context),
          instance_(context.instance_id),
          incarnation_(
              common::correlation_id_hex(common::make_correlation_id())),
          metrics_(context.metrics) {
        const auto& discovery = context.config.host().service_discovery;
        if (!discovery.enabled || !discovery.required)
            throw std::invalid_argument(
                "dynamic Scene requires mandatory service discovery");
        std::size_t listeners = 0;
        for (const auto& transport : context.config.edge().transports) {
            if (!transport.enabled) continue;
            ++listeners;
            if (transport.protocol != network::TransportProtocol::TlsTcp ||
                !transport.tls ||
                transport.tls->alpn != common::scene_internal_alpn ||
                transport.tls->client_ca_file.empty() ||
                transport.tls->allowed_client_identities.empty() ||
                transport.max_payload_size > common::scene_envelope_max ||
                transport.max_sessions > 64)
                throw std::invalid_argument(
                    "dynamic Scene requires bounded mutual TLS");
        }
        if (listeners != 1)
            throw std::invalid_argument("Scene requires one TLS listener");
        auto data = common::parse_player_data_config(
            service_host::lua_root(context.config));
        common::resolve_player_data_paths(data, context.config.config_root());
        if (data.uri.empty())
            throw std::invalid_argument("dynamic Scene requires player_data");
        store_ = std::make_unique<common::MongoPlayerDataStore>(
            data.uri, data.database, data.options);
        metrics_.counter_add("scene_drain_attempts_total", 0);
        metrics_.counter_add(
            "scene_drain_failures_total", 0, {{"reason", "timeout"}});
    }
    ~DynamicSceneModule() override { pool_.stop(); }
    void started() override {
        // 宿主已经注册；启动边界允许同步 IO，业务帧不访问数据库。
        const auto expected = store_->scene_instance_state(instance_);
        const common::SceneInstanceState desired{
            instance_, incarnation_, false};
        bool published = false;
        try {
            published =
                store_->compare_exchange_scene_instance(expected, desired);
        } catch (const common::SceneWriteUncertain&) {
            published = store_->scene_instance_state(instance_) ==
                        std::optional(desired);
            if (!published) throw;
        }
        if (!published)
            throw std::runtime_error(
                "Scene business incarnation initialization conflicted");
        initialized_ = true;
    }
    bool ready() const noexcept override { return initialized_ && !disabled_; }
    void tick(const service_host::ServiceTickContext&) override {
        for (auto& completed : pool_.drain(64)) {
            const auto found = in_flight_.find(completed.id);
            if (found == in_flight_.end()) continue;
            auto call = std::move(found->second);
            in_flight_.erase(found);
            if (call.request.operation() == internal::OPERATION_SET_DRAINING)
                --control_pending_;
            const auto peer = peers_.find(call.connection);
            if (peer == peers_.end() || peer->second.busy != completed.id)
                continue;
            peer->second.busy = 0;
            auto reply = base_reply(call.request, internal::STATUS_UNAVAILABLE);
            if (!completed.result.failed && Clock::now() < call.deadline) {
                merge(completed.result);
                reply = handle(call);
            }
            send(call.connection, reply, call.correlation);
        }
        for (auto& event : runtime().drain_events(256)) {
            if (event.kind == edge::EdgeEventKind::ConnectionClosed) {
                const auto peer = peers_.find(event.connection_id);
                if (peer != peers_.end()) {
                    for (const auto& queued : peer->second.waiting)
                        if (queued.request.operation() ==
                            internal::OPERATION_SET_DRAINING)
                            --control_pending_;
                    if (peer->second.busy != 0) {
                        const auto pending = in_flight_.find(peer->second.busy);
                        if (pending != in_flight_.end()) {
                            if (pending->second.request.operation() ==
                                internal::OPERATION_SET_DRAINING)
                                --control_pending_;
                            in_flight_.erase(pending);
                        }
                        pool_.cancel(peer->second.busy);
                    }
                    peers_.erase(peer);
                }
                for (auto& [shard, value] : shards_) {
                    static_cast<void>(shard);
                    value->closed(event.connection_id.value);
                }
            } else if (event.kind == edge::EdgeEventKind::MessageReceived) {
                const auto request =
                    common::decode_scene_request(event.payload);
                const auto correlation =
                    common::scene_internal_request_id(event.payload);
                auto peer = peers_.find(event.connection_id);
                if (!request || !correlation ||
                    (peer == peers_.end() &&
                     request->operation() != internal::OPERATION_STATUS) ||
                    (peer != peers_.end() &&
                     peer->second.realm_incarnation !=
                         request->realm_incarnation()) ||
                    (request->scene_instance_id() != instance_) ||
                    (request->scene_incarnation() != incarnation_ &&
                     !(request->operation() == internal::OPERATION_STATUS &&
                       request->scene_incarnation() == "discover"))) {
                    static_cast<void>(runtime().try_close(event.connection_id));
                    continue;
                }
                if (peer == peers_.end())
                    peer =
                        peers_
                            .emplace(
                                event.connection_id,
                                Peer{
                                    request->realm_incarnation(), false, 0, {}})
                            .first;
                if (!initialized_) {
                    send(
                        event.connection_id,
                        base_reply(*request, internal::STATUS_UNAVAILABLE),
                        *correlation);
                    continue;
                }
                if (peer->second.waiting.size() >= 8) {
                    send(
                        event.connection_id,
                        base_reply(*request, internal::STATUS_BUSY),
                        *correlation);
                    continue;
                }
                if (request->operation() == internal::OPERATION_SET_DRAINING) {
                    if (!request->draining() || request->drain_id().empty() ||
                        request->drain_timeout_ms() < 100) {
                        send(
                            event.connection_id,
                            base_reply(*request, internal::STATUS_INVALID),
                            *correlation);
                        continue;
                    }
                    const bool retry =
                        phase() == internal::DRAIN_PHASE_FAILED &&
                        request->drain_id() != drain_id_;
                    if (drain_id_.empty() || retry) {
                        drain_id_ = request->drain_id();
                        drain_deadline_ =
                            Clock::now() + std::chrono::milliseconds(
                                               request->drain_timeout_ms());
                        failed_counted_ = false;
                        metrics_.counter_add("scene_drain_attempts_total");
                    }
                    draining_ = true;
                    for (auto& [shard, value] : shards_) {
                        static_cast<void>(shard);
                        value->begin_draining();
                    }
                }
                peer->second.waiting.push_back(
                    {event.connection_id,
                     *request,
                     *correlation,
                     Clock::now() + std::chrono::seconds(3)});
                if (request->operation() == internal::OPERATION_SET_DRAINING)
                    ++control_pending_;
            }
        }
        for (auto& [id, peer] : peers_) {
            static_cast<void>(id);
            if (peer.busy != 0 || peer.waiting.empty()) continue;
            auto call = std::move(peer.waiting.front());
            peer.waiting.pop_front();
            if (Clock::now() >= call.deadline) {
                if (call.request.operation() ==
                    internal::OPERATION_SET_DRAINING)
                    --control_pending_;
                send(
                    call.connection,
                    base_reply(call.request, internal::STATUS_UNAVAILABLE),
                    call.correlation);
                continue;
            }
            const auto job = next_job_++;
            const auto submitted =
                pool_.try_submit(job, [this, request = call.request] {
                    Checked checked;
                    try {
                        if (request.operation() ==
                            internal::OPERATION_SET_DRAINING) {
                            auto state =
                                store_->scene_instance_state(instance_);
                            if (!state || state->incarnation != incarnation_)
                                throw std::runtime_error(
                                    "Scene business incarnation no longer owned");
                            if (!state->draining) {
                                auto desired = *state;
                                desired.draining = true;
                                if (!store_->compare_exchange_scene_instance(
                                        state, desired)) {
                                    state =
                                        store_->scene_instance_state(instance_);
                                    if (!state ||
                                        state->incarnation != incarnation_ ||
                                        !state->draining)
                                        throw std::runtime_error(
                                            "Scene instance drain CAS conflict");
                                }
                            }
                        }
                        const bool control =
                            request.operation() == internal::OPERATION_STATUS ||
                            request.operation() ==
                                internal::OPERATION_SET_DRAINING;
                        if (control) {
                            const auto state =
                                store_->scene_instance_state(instance_);
                            if (!state || state->incarnation != incarnation_)
                                throw std::runtime_error(
                                    "Scene business incarnation no longer current");
                            checked.instance_checked = true;
                            checked.drain_fenced = state->draining;
                        }
                        const auto target = request.has_route()
                                                ? request.route().shard_id()
                                                : request.shard_id();
                        for (std::size_t i = 0; i < 2; ++i) {
                            if (!control && target != i + 1) continue;
                            checked.observed[i] = true;
                            auto record = store_->shard_state(1, i + 1);
                            checked.records[i] = std::move(record);
                        }
                    } catch (...) {
                        checked.failed = true;
                    }
                    return checked;
                });
            if (submitted != concurrency::WorkSubmitResult::Submitted) {
                if (call.request.operation() ==
                    internal::OPERATION_SET_DRAINING)
                    --control_pending_;
                metrics_.counter_add("scene_queue_rejections_total");
                send(
                    call.connection,
                    base_reply(call.request, internal::STATUS_BUSY),
                    call.correlation);
                continue;
            }
            peer.busy = job;
            in_flight_.emplace(job, std::move(call));
        }
        std::size_t active = 0, prepared = 0;
        for (auto& [shard, value] : shards_) {
            static_cast<void>(shard);
            value->expire();
            active += value->active_members();
            prepared += value->prepared_members();
            for (const auto& event : value->drain_events()) {
                for (const auto& [id, peer] : peers_) {
                    if (!peer.accepted) continue;
                    common::ScenePush push;
                    push.set_realm_incarnation(peer.realm_incarnation);
                    push.set_scene_instance_id(instance_);
                    push.set_scene_incarnation(incarnation_);
                    *push.mutable_event() = event;
                    if (runtime().try_send(id, common::encode(push)) !=
                        edge::QueueResult::Queued)
                        static_cast<void>(runtime().try_close(id));
                }
            }
        }
        const auto state = phase();
        disabled_ = state == internal::DRAIN_PHASE_COMPLETE;
        if (state == internal::DRAIN_PHASE_FAILED && !failed_counted_) {
            failed_counted_ = true;
            metrics_.counter_add(
                "scene_drain_failures_total", 1, {{"reason", "timeout"}});
        }
        metrics_.gauge_set("scene_shard_members", static_cast<double>(active));
        metrics_.gauge_set(
            "scene_prepared_members", static_cast<double>(prepared));
        metrics_.gauge_set("scene_draining", draining_ ? 1 : 0);
        metrics_.gauge_set(
            "scene_owned_shards", static_cast<double>(shards_.size()));
        metrics_.gauge_set(
            "scene_drain_remaining_members",
            draining_ ? static_cast<double>(active + prepared) : 0);
        metrics_.gauge_set(
            "scene_drain_failed",
            state == internal::DRAIN_PHASE_FAILED ? 1 : 0);
    }

private:
    bool mine(const std::optional<common::SceneShardRecord>& record) const {
        return record && record->owner &&
               record->owner->instance_id == instance_ &&
               record->owner->incarnation == incarnation_;
    }
    void merge(const Checked& checked) {
        if (checked.instance_checked && checked.drain_fenced) {
            drain_fenced_ = true;
            draining_ = true;
        }
        for (std::size_t i = 0; i < 2; ++i) {
            if (!checked.observed[i] || !checked.records[i]) continue;
            const auto& next = *checked.records[i];
            if (known_[i] && (known_[i]->version > next.version ||
                              (known_[i]->version == next.version &&
                               known_[i]->draining && !next.draining)))
                continue;
            const bool changed =
                !known_[i] || known_[i]->version != next.version;
            known_[i] = next;
            if (!mine(known_[i]))
                shards_.erase(i + 1);
            else {
                if (changed || !shards_.contains(i + 1))
                    shards_[i + 1] = std::make_unique<SceneShard>(
                        instance_, incarnation_, i + 1, next.version);
                if (next.draining) draining_ = true;
                if (draining_) shards_.at(i + 1)->begin_draining();
            }
        }
    }
    internal::DrainPhase phase() const {
        if (!draining_) return internal::DRAIN_PHASE_IDLE;
        if (failed_counted_) return internal::DRAIN_PHASE_FAILED;
        if (drain_fenced_ && control_pending_ == 0 && shards_.empty())
            return internal::DRAIN_PHASE_COMPLETE;
        return Clock::now() >= drain_deadline_ ? internal::DRAIN_PHASE_FAILED
                                               : internal::DRAIN_PHASE_RUNNING;
    }
    common::SceneReply base_reply(
        const common::SceneRequest& request, internal::Status status) const {
        common::SceneReply reply;
        reply.set_realm_incarnation(request.realm_incarnation());
        reply.set_scene_instance_id(instance_);
        reply.set_scene_incarnation(incarnation_);
        reply.set_operation(request.operation());
        reply.set_status(status);
        return reply;
    }
    common::SceneReply handle(const Pending& call) {
        const auto& request = call.request;
        const auto op = request.operation();
        if (op == internal::OPERATION_STATUS ||
            op == internal::OPERATION_SET_DRAINING) {
            auto reply = base_reply(request, internal::STATUS_OK);
            reply.set_dynamic(true);
            reply.set_business_ready(phase() != internal::DRAIN_PHASE_COMPLETE);
            reply.set_draining(draining_);
            reply.set_drain_phase(phase());
            reply.set_drain_id(drain_id_);
            std::uint32_t active = 0, prepared = 0;
            for (const auto& [id, shard] : shards_) {
                auto* value = reply.add_shards();
                value->set_realm_id(1);
                value->set_shard_id(id);
                value->set_version(
                    known_[static_cast<std::size_t>(id - 1)]->version);
                value->set_active_members(
                    static_cast<std::uint32_t>(shard->active_members()));
                value->set_prepared_members(
                    static_cast<std::uint32_t>(shard->prepared_members()));
                active += value->active_members();
                prepared += value->prepared_members();
            }
            reply.set_active_members(active);
            reply.set_prepared_members(prepared);
            return reply;
        }
        const auto id = request.has_route() ? request.route().shard_id()
                                            : request.shard_id();
        const auto version = request.has_route()
                                 ? request.route().shard_version()
                                 : request.shard_version();
        if (id == 0 || id > 2 ||
            !mine(known_[static_cast<std::size_t>(id - 1)]) ||
            known_[static_cast<std::size_t>(id - 1)]->version != version ||
            !shards_.contains(id)) {
            metrics_.counter_add("scene_stale_rejections_total");
            return base_reply(request, internal::STATUS_STALE_BINDING);
        }
        return shards_.at(id)->handle(call.connection.value, request);
    }
    void send(
        Id id, const common::SceneReply& reply, std::uint64_t correlation) {
        const auto peer = peers_.find(id);
        if (peer == peers_.end()) return;
        const auto payload = common::encode(reply, correlation);
        const auto result = peer->second.accepted
                                ? runtime().try_send(id, payload)
                                : runtime().try_accept(id, payload);
        if (result == edge::QueueResult::Queued)
            peer->second.accepted = true;
        else
            static_cast<void>(runtime().try_close(id));
    }
    std::string instance_, incarnation_;
    observability::MetricsRegistry& metrics_;
    std::unique_ptr<common::MongoPlayerDataStore> store_;
    concurrency::BoundedWorkPool<Checked> pool_{4, 64};
    std::unordered_map<Id, Peer> peers_;
    std::map<std::uint64_t, Pending> in_flight_;
    std::array<std::optional<common::SceneShardRecord>, 2> known_;
    std::map<std::uint64_t, std::unique_ptr<SceneShard>> shards_;
    std::uint64_t next_job_{1};
    bool draining_{false}, failed_counted_{false}, drain_fenced_{false};
    std::atomic_bool disabled_{false};
    std::atomic_bool initialized_{false};
    std::size_t control_pending_{0};
    std::string drain_id_;
    Clock::time_point drain_deadline_{Clock::time_point::max()};
};
}  // namespace
std::unique_ptr<service_host::ServiceModule> make_dynamic_scene_module(
    const service_host::ServiceModuleContext& context) {
    return std::make_unique<DynamicSceneModule>(context);
}
}  // namespace realm::game::scene
