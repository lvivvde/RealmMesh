#include "realmmesh/game/realm/realm_scenes.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <thread>
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/game/scene/scene_shard.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"
#include "realmmesh/observability/metrics_registry.hpp"

namespace realm::game::realm {
namespace {
using namespace std::chrono_literals;
namespace internal = ::realmmesh::protocol::scene_internal::v1;
class SceneStore final : public common::RealmSceneStore {
public:
    std::optional<common::SceneStateRecord> initialize_scene(
        const common::SceneCharacterKey& key) override {
        initialize_entered = true;
        while (block_initialize) std::this_thread::sleep_for(1ms);
        const std::scoped_lock lock(mutex);
        auto& record = records[key.character_id];
        if (!record) record = common::SceneStateRecord{.character = key};
        return record;
    }
    std::optional<common::SceneStateRecord> scene_state(
        const common::SceneCharacterKey& key) const override {
        const std::scoped_lock lock(mutex);
        const auto found = records.find(key.character_id);
        return found == records.end() ? std::nullopt : found->second;
    }
    common::SceneWriteResult commit_scene(
        const common::SceneWrite& write) override {
        commit_entered = true;
        while (block_commit) std::this_thread::sleep_for(1ms);
        const std::scoped_lock lock(mutex);
        auto& record = records[write.character.character_id];
        if (record->last_result &&
            record->last_result->command == write.command &&
            record->owner == std::optional(write.owner))
            return {common::SceneWriteOutcome::Replayed, record};
        if (record->binding_epoch != write.expected_epoch ||
            record->owner != write.expected_owner)
            return {common::SceneWriteOutcome::StaleBinding, record};
        if (reject_commit.exchange(false))
            return {common::SceneWriteOutcome::StaleBinding, record};
        if (uncertain_before_commit.exchange(false))
            throw common::SceneWriteUncertain("before response unknown");
        ++commits;
        if (write.command.type != common::SceneCommandType::Move)
            ++record->binding_epoch;
        record->owner = write.owner;
        record->confirmed_location = write.location;
        record->last_result = common::SceneStoredResult{
            write.command, write.location, record->binding_epoch, write.owner};
        if (uncertain.exchange(false))
            throw common::SceneWriteUncertain("injected lost response");
        return {common::SceneWriteOutcome::Committed, record};
    }
    bool revoke_scene(
        const common::SceneCharacterKey& key,
        std::uint64_t epoch,
        const common::SceneOwner& owner) override {
        const std::scoped_lock lock(mutex);
        auto& record = records[key.character_id];
        if (!record || record->binding_epoch != epoch ||
            record->owner != std::optional(owner))
            return false;
        ++record->binding_epoch;
        record->owner.reset();
        ++revokes;
        return true;
    }
    std::atomic_bool block_initialize{false}, initialize_entered{false},
        block_commit{false}, commit_entered{false}, uncertain{false};
    std::atomic_bool reject_commit{false}, uncertain_before_commit{false};
    std::atomic_int commits{0}, revokes{0};
    mutable std::mutex mutex;
    std::map<std::uint64_t, std::optional<common::SceneStateRecord>> records;
    std::optional<common::SceneStateRecord>& record{records[10]};
};
class SceneOutbox final : public RealmOutbox {
public:
    void send(
        edge::EdgeConnectionId id, std::vector<std::byte> payload) override {
        sent.emplace_back(id, std::move(payload));
    }
    void close(edge::EdgeConnectionId id) override { closed.push_back(id); }
    std::vector<std::pair<edge::EdgeConnectionId, std::vector<std::byte>>> sent;
    std::vector<edge::EdgeConnectionId> closed;
};
class RealmScenesTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto tls =
            std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY);
        network::TransportConfig transport;
        transport.name = "internal";
        transport.listen_address = "127.0.0.1";
        transport.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = (tls / "scene.pem").string(),
            .private_key_file = (tls / "scene.key").string(),
            .alpn = common::scene_internal_alpn,
            .client_ca_file = (tls / "ca.pem").string(),
            .allowed_client_identities = {"realm-dev-01.realm.service"}};
        auto list =
            network::TransportFactory::create_enabled(std::vector{transport});
        server_ = std::move(list.front());
        config_.scene.enabled = true;
        config_.scene.port = server_->local_endpoint().port;
        config_.scene.ca_file = (tls / "ca.pem").string();
        config_.scene.certificate_file = (tls / "realm.pem").string();
        config_.scene.private_key_file = (tls / "realm.key").string();
        server_thread_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                for (auto& event : server_->poll_once(1ms)) {
                    if (event.kind ==
                        network::TransportEventKind::SessionClosed)
                        shard_.closed(event.session_id);
                    if (event.kind !=
                        network::TransportEventKind::MessageReceived)
                        continue;
                    const auto request =
                        common::decode_scene_request(event.payload);
                    if (!request) continue;
                    last_session_ = event.session_id;
                    last_realm_incarnation_ = request->realm_incarnation();
                    const auto reply =
                        shard_.handle(event.session_id, *request);
                    const auto delay =
                        request->operation() == internal::OPERATION_REMOVE
                            ? remove_delay_ms_.load()
                        : request->operation() == internal::OPERATION_SNAPSHOT
                            ? snapshot_delay_ms_.load()
                            : 0;
                    if (delay > 0)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(delay));
                    if (!((drop_activation_ &&
                           request->operation() ==
                               internal::OPERATION_ACTIVATE) ||
                          (drop_remove_ &&
                           request->operation() == internal::OPERATION_REMOVE)))
                        static_cast<void>(server_->send(
                            event.session_id,
                            common::encode(
                                reply,
                                *common::scene_internal_request_id(
                                    event.payload))));
                    if (request->operation() == internal::OPERATION_REMOVE)
                        ++removals_completed_;
                    if (request->operation() == internal::OPERATION_SNAPSHOT &&
                        delay > 0)
                        ++delayed_snapshots_completed_;
                    // 此测试服务不广播;SceneShard
                    // 单测与多进程旅程验证真实推送。
                    static_cast<void>(shard_.drain_events());
                }
                if (inject_gap_.exchange(false) &&
                    last_session_ != network::invalid_session_id) {
                    common::ScenePush push;
                    push.set_realm_incarnation(last_realm_incarnation_);
                    push.set_scene_instance_id("scene-dev-01");
                    push.set_scene_incarnation("scene-boot");
                    auto* event = push.mutable_event();
                    event->set_shard_id(1);
                    event->set_shard_incarnation("scene-boot");
                    event->set_event_seq(3);
                    event->set_kind(common::SceneEvent::KIND_MOVE);
                    event->mutable_member()->set_character_id(10);
                    event->mutable_member()->set_binding_epoch(1);
                    event->mutable_member()->mutable_location()->set_shard_id(
                        1);
                    static_cast<void>(
                        server_->send(last_session_, common::encode(push)));
                }
            }
        });
        scenes_ =
            std::make_unique<RealmScenes>(store_, outbox_, config_, metrics_);
        scenes_->selected({1}, {7001, 1, 10});
    }
    void TearDown() override {
        store_.block_initialize = false;
        store_.block_commit = false;
        scenes_.reset();
        server_thread_.request_stop();
        server_thread_.join();
        server_.reset();
    }
    template <typename Predicate>
    void wait(Predicate predicate, std::chrono::seconds timeout = 5s) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate() && std::chrono::steady_clock::now() < deadline) {
            scenes_->complete(1024);
            std::this_thread::sleep_for(1ms);
        }
        ASSERT_TRUE(predicate());
    }
    void enter(
        edge::EdgeConnectionId id,
        std::uint64_t seq,
        std::uint64_t correlation) {
        common::EnterScene request;
        request.set_scene_seq(seq);
        ASSERT_TRUE(scenes_->handle(id, common::encode(request, correlation)));
    }
    std::optional<std::vector<std::byte>> reply(std::uint64_t correlation) {
        for (const auto& [id, bytes] : outbox_.sent) {
            static_cast<void>(id);
            if (common::scene_request_id(bytes) == correlation ||
                common::edge_request_id(bytes) == correlation)
                return bytes;
        }
        return std::nullopt;
    }
    SceneStore store_;
    SceneOutbox outbox_;
    RealmConfig config_;
    observability::MetricsRegistry metrics_;
    scene::SceneShard shard_{"scene-dev-01", "scene-boot"};
    std::unique_ptr<network::IMessageTransport> server_;
    std::atomic_bool drop_activation_{false}, drop_remove_{false},
        inject_gap_{false};
    std::atomic_int remove_delay_ms_{0}, snapshot_delay_ms_{0};
    std::atomic_int removals_completed_{0}, delayed_snapshots_completed_{0};
    network::SessionId last_session_{network::invalid_session_id};
    std::string last_realm_incarnation_;
    std::jthread server_thread_;
    std::unique_ptr<RealmScenes> scenes_;
};
TEST_F(
    RealmScenesTest,
    ClosedDuringWriteNeverRepliesAndNewBindingSurvivesCleanup) {
    store_.block_commit = true;
    enter({1}, 1, 11);
    wait([&] {
        return store_.commit_entered.load();
    });
    const auto start = std::chrono::steady_clock::now();
    scenes_->closed({1});
    scenes_->selected({2}, {7001, 1, 10});
    enter({2}, 2, 22);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 100ms);
    store_.block_commit = false;
    wait([&] {
        return reply(22).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(22)));
    EXPECT_FALSE(reply(11));
    EXPECT_EQ(store_.commits, 2);
    EXPECT_EQ(store_.revokes, 1);
    const auto state = store_.scene_state({7001, 1, 10});
    ASSERT_TRUE(state->owner);
    EXPECT_EQ(state->owner->session_binding, 2);
    scenes_->closed({1});
    scenes_->complete(1024);
    EXPECT_EQ(store_.scene_state({7001, 1, 10})->owner, state->owner);
}
TEST_F(
    RealmScenesTest, DelayedRemovalKeepsOtherObserverAndSharedChannelActive) {
    enter({1}, 1, 11);
    wait([&] {
        return reply(11).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(11)));
    scenes_->selected({2}, {7002, 1, 20});
    enter({2}, 1, 21);
    wait([&] {
        return reply(21).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(21)));
    remove_delay_ms_ = 250;
    scenes_->closed({2});
    wait([&] {
        return removals_completed_.load() == 1;
    });
    ASSERT_TRUE(
        scenes_->handle({1}, common::encode(common::GetSceneState{}, 12)));
    wait([&] {
        return reply(12).has_value();
    });
    const auto state = common::decode_scene_state(*reply(12));
    ASSERT_TRUE(state);
    EXPECT_EQ(state->phase(), ::realmmesh::protocol::scene::v1::PHASE_ACTIVE);
    ASSERT_EQ(state->snapshot().members_size(), 1);
    EXPECT_EQ(state->snapshot().members(0).character_id(), 10);
}
TEST_F(RealmScenesTest, DelayedHealthSnapshotKeepsActiveBindingAvailable) {
    enter({1}, 1, 11);
    wait([&] {
        return reply(11).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(11)));
    snapshot_delay_ms_ = 350;
    wait([&] {
        return delayed_snapshots_completed_.load() >= 1;
    });
    ASSERT_TRUE(
        scenes_->handle({1}, common::encode(common::GetSceneState{}, 12)));
    wait([&] {
        return reply(12).has_value();
    });
    const auto state = common::decode_scene_state(*reply(12));
    ASSERT_TRUE(state);
    EXPECT_EQ(state->phase(), ::realmmesh::protocol::scene::v1::PHASE_ACTIVE);
    EXPECT_EQ(state->snapshot().members_size(), 1);
}
TEST_F(
    RealmScenesTest,
    LostWriteResponseIsReadBackBeforeActivationAndReplayDoesNotWrite) {
    store_.uncertain = true;
    enter({1}, 1, 11);
    wait([&] {
        return reply(11).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(11)));
    EXPECT_EQ(store_.commits, 1);
    enter({1}, 1, 12);
    wait([&] {
        return reply(12).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(12)));
    EXPECT_TRUE(common::decode_scene_command_result(*reply(12))->replayed());
    EXPECT_EQ(store_.commits, 1);
}
TEST_F(
    RealmScenesTest,
    ActivationTimeoutReportsUnknownAndSameCommandRecoversReceipt) {
    drop_activation_ = true;
    enter({1}, 1, 11);
    wait([&] {
        return reply(11).has_value();
    });
    ASSERT_TRUE(common::decode_edge_error(*reply(11)));
    EXPECT_EQ(common::decode_edge_error(*reply(11))->code(), 3105);
    EXPECT_EQ(store_.commits, 1);
    drop_activation_ = false;
    enter({1}, 1, 12);
    wait([&] {
        return reply(12).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(12)));
    EXPECT_EQ(store_.commits, 1);
}
TEST_F(
    RealmScenesTest,
    EventGapFetchesSnapshotAndRequestsClientResynchronization) {
    enter({1}, 1, 11);
    wait([&] {
        return reply(11).has_value();
    });
    inject_gap_ = true;
    wait([&] {
        for (const auto& [id, bytes] : outbox_.sent) {
            static_cast<void>(id);
            if (common::decode_scene_unavailable(bytes)) return true;
        }
        return false;
    });
    for (const auto& [id, bytes] : outbox_.sent) {
        static_cast<void>(id);
        EXPECT_FALSE(common::decode_scene_event(bytes));
    }
    EXPECT_NE(
        metrics_.render().find("scene_snapshot_resync_total 1"),
        std::string::npos);
    ASSERT_TRUE(
        scenes_->handle({1}, common::encode(common::GetSceneState{}, 12)));
    wait([&] {
        return reply(12).has_value();
    });
    const auto state = common::decode_scene_state(*reply(12));
    ASSERT_TRUE(state);
    EXPECT_EQ(state->snapshot().event_seq(), 1);
    EXPECT_EQ(state->snapshot().members_size(), 1);
}
TEST_F(
    RealmScenesTest, InstanceCapacityIncludesQueuedRunningAndUndrainedResults) {
    store_.block_initialize = true;
    enter({1}, 1, 1);
    wait([&] {
        return store_.initialize_entered.load();
    });
    for (std::uint64_t i = 2; i <= 65; ++i) {
        scenes_->selected({i}, {7000 + i, 1, i + 10});
        enter({i}, 1, i);
    }
    ASSERT_TRUE(reply(65));
    EXPECT_EQ(common::decode_edge_error(*reply(65))->code(), 429);
    store_.block_initialize = false;
}
TEST_F(RealmScenesTest, QueueLimitAndWaitingTimeAreIncludedInCommandDeadline) {
    store_.block_initialize = true;
    enter({1}, 1, 11);
    wait([&] {
        return store_.initialize_entered.load();
    });
    for (std::uint64_t i = 12; i <= 19; ++i) enter({1}, 1, i);
    enter({1}, 1, 20);
    ASSERT_TRUE(reply(20));
    EXPECT_EQ(common::decode_edge_error(*reply(20))->code(), 429);
    // 已入队指令的三秒期限在阻塞初始化时仍计时。
    wait([&] {
        return reply(19).has_value();
    });
    EXPECT_TRUE(store_.block_initialize.load());
    store_.block_initialize = false;
    EXPECT_EQ(common::decode_edge_error(*reply(19))->code(), 3104);
    EXPECT_EQ(store_.commits, 0);
}
class SceneTransferTest : public RealmScenesTest {
protected:
    void SetUp() override {
        RealmScenesTest::SetUp();
        scenes_.reset();
        start_b("scene-boot-b");
        config_.scene.targets.push_back({.shard_id = 2, .port = port_b_});
        scenes_ =
            std::make_unique<RealmScenes>(store_, outbox_, config_, metrics_);
        scenes_->selected({1}, {7001, 1, 10});
    }
    void start_b(std::string incarnation) {
        const auto tls =
            std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY);
        network::TransportConfig config;
        config.name = "internal-b";
        config.listen_address = "127.0.0.1";
        config.listen_port = port_b_;
        config.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = (tls / "scene2.pem").string(),
            .private_key_file = (tls / "scene2.key").string(),
            .alpn = common::scene_internal_alpn,
            .client_ca_file = (tls / "ca.pem").string(),
            .allowed_client_identities = {"realm-dev-01.realm.service"}};
        auto transports =
            network::TransportFactory::create_enabled(std::vector{config});
        server_b_ = std::move(transports.front());
        port_b_ = server_b_->local_endpoint().port;
        shard_b_ = std::make_unique<scene::SceneShard>(
            "scene-dev-02", std::move(incarnation), 2);
        thread_b_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                for (const auto& event : server_b_->poll_once(1ms)) {
                    if (event.kind ==
                        network::TransportEventKind::SessionClosed)
                        shard_b_->closed(event.session_id);
                    if (event.kind !=
                        network::TransportEventKind::MessageReceived)
                        continue;
                    const auto request =
                        common::decode_scene_request(event.payload);
                    if (!request) continue;
                    auto reply = shard_b_->handle(event.session_id, *request);
                    if (request->operation() == internal::OPERATION_PREPARE) {
                        ++prepares_b_;
                        if (reject_prepare_b_) {
                            reply.set_status(internal::STATUS_BUSY);
                            reply.clear_route();
                            reply.clear_location();
                            reply.set_scene_seq(0);
                        }
                    }
                    if (!((drop_prepare_b_ &&
                           request->operation() ==
                               internal::OPERATION_PREPARE) ||
                          (drop_activation_b_ &&
                           request->operation() ==
                               internal::OPERATION_ACTIVATE)))
                        static_cast<void>(server_b_->send(
                            event.session_id,
                            common::encode(
                                reply,
                                *common::scene_internal_request_id(
                                    event.payload))));
                    static_cast<void>(shard_b_->drain_events());
                }
                active_b_ = shard_b_->active_members();
            }
        });
    }
    void stop_b() {
        if (thread_b_.joinable()) {
            thread_b_.request_stop();
            thread_b_.join();
        }
        server_b_.reset();
        shard_b_.reset();
    }
    void TearDown() override {
        drop_prepare_b_ = false;
        drop_activation_b_ = false;
        drop_remove_ = false;
        RealmScenesTest::TearDown();
        stop_b();
    }
    void enter_a() {
        enter({1}, 1, 11);
        wait([&] {
            return reply(11).has_value();
        });
        ASSERT_TRUE(common::decode_scene_command_result(*reply(11)));
    }
    void transfer(
        std::uint64_t seq, std::uint64_t target, std::uint64_t correlation) {
        common::TransferScene request;
        request.set_scene_seq(seq);
        request.set_target_shard_id(target);
        ASSERT_TRUE(scenes_->handle({1}, common::encode(request, correlation)));
    }
    void move(
        edge::EdgeConnectionId id,
        std::uint64_t seq,
        std::uint64_t correlation) {
        common::MoveInScene request;
        request.set_scene_seq(seq);
        request.set_x(10);
        ASSERT_TRUE(scenes_->handle(id, common::encode(request, correlation)));
    }
    void expect_source() {
        const auto value = store_.scene_state({7001, 1, 10});
        ASSERT_TRUE(value);
        EXPECT_EQ(value->confirmed_location->shard_id, 1);
        EXPECT_EQ(value->last_result->command.seq, 1);
        EXPECT_EQ(value->owner->scene_instance_id, "scene-dev-01");
        move({1}, 2, 13);
        wait([&] {
            return reply(13).has_value();
        });
        EXPECT_TRUE(common::decode_scene_command_result(*reply(13)));
    }
    std::unique_ptr<network::IMessageTransport> server_b_;
    std::unique_ptr<scene::SceneShard> shard_b_;
    std::jthread thread_b_;
    std::uint16_t port_b_{0};
    std::atomic_bool reject_prepare_b_{false}, drop_prepare_b_{false},
        drop_activation_b_{false};
    std::atomic_size_t active_b_{0}, prepares_b_{0};
};
TEST_F(SceneTransferTest, TransferCommitsNewEpochAndReplayCannotReenterSource) {
    enter_a();
    const auto old = store_.scene_state({7001, 1, 10});
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(12)));
    const auto value = store_.scene_state({7001, 1, 10});
    EXPECT_EQ(value->binding_epoch, old->binding_epoch + 1);
    EXPECT_EQ(value->owner->scene_instance_id, "scene-dev-02");
    EXPECT_EQ(value->confirmed_location->shard_id, 2);
    const auto count = store_.commits.load();
    transfer(2, 2, 14);
    wait([&] {
        return reply(14).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(14)));
    EXPECT_TRUE(common::decode_scene_command_result(*reply(14))->replayed());
    EXPECT_EQ(store_.commits, count);
    common::SceneWrite stale{
        old->character,
        old->binding_epoch,
        old->owner,
        *old->owner,
        {common::SceneCommandType::Move, 3, 10, 0},
        {1, 10, 0}};
    EXPECT_EQ(
        store_.commit_scene(stale).outcome,
        common::SceneWriteOutcome::StaleBinding);
}
TEST_F(SceneTransferTest, DisallowedOrSameShardDoesNotConsumeSequence) {
    enter_a();
    transfer(2, 3, 12);
    wait([&] {
        return reply(12).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(12))->code(), 3102);
    transfer(2, 1, 14);
    wait([&] {
        return reply(14).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(14))->code(), 3102);
    expect_source();
}
TEST_F(SceneTransferTest, PreparationRejectedBeforeCommitKeepsSourceMovable) {
    enter_a();
    reject_prepare_b_ = true;
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(12))->code(), 429);
    EXPECT_EQ(active_b_, 0);
    expect_source();
}
TEST_F(
    SceneTransferTest, PreparationReplyLostKeepsSourceAndCandidateInvisible) {
    enter_a();
    drop_prepare_b_ = true;
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(12))->code(), 3104);
    EXPECT_GE(prepares_b_, 1);
    EXPECT_EQ(active_b_, 0);
    expect_source();
}
TEST_F(SceneTransferTest, TargetAbsentBeforePreparationKeepsSource) {
    enter_a();
    stop_b();
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(12))->code(), 3104);
    expect_source();
}
TEST_F(
    SceneTransferTest, PreparedTargetStaysInvisibleUntilOldMoveAndCasSettle) {
    enter_a();
    store_.block_commit = true;
    store_.commit_entered = false;
    move({1}, 2, 12);
    wait([&] {
        return store_.commit_entered.load();
    });
    transfer(3, 2, 13);
    EXPECT_EQ(prepares_b_, 0);
    store_.block_commit = false;
    wait([&] {
        return reply(13).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(13)));
    EXPECT_EQ(store_.scene_state({7001, 1, 10})->last_result->command.seq, 3);
}
TEST_F(
    SceneTransferTest, OldSourceEventsCannotResynchronizeOrMoveTargetObserver) {
    enter_a();
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(12)));
    inject_gap_ = true;
    move({1}, 3, 13);
    wait([&] {
        return reply(13).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(13)));
    ASSERT_TRUE(
        scenes_->handle({1}, common::encode(common::GetSceneState{}, 14)));
    wait([&] {
        return reply(14).has_value();
    });
    const auto state = common::decode_scene_state(*reply(14));
    ASSERT_TRUE(state);
    EXPECT_EQ(state->snapshot().shard_id(), 2);
    EXPECT_EQ(state->phase(), ::realmmesh::protocol::scene::v1::PHASE_ACTIVE);
    EXPECT_EQ(
        metrics_.render().find("scene_snapshot_resync_total"),
        std::string::npos);
}
TEST_F(SceneTransferTest, CasRejectedAfterPreparationKeepsSource) {
    enter_a();
    store_.reject_commit = true;
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(12))->code(), 3106);
    EXPECT_EQ(active_b_, 0);
    expect_source();
}
TEST_F(SceneTransferTest, LostCasResponseReadsTargetReceiptBeforeActivation) {
    enter_a();
    store_.uncertain = true;
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(12)));
    EXPECT_EQ(store_.commits, 2);
    EXPECT_EQ(
        store_.scene_state({7001, 1, 10})->confirmed_location->shard_id, 2);
    EXPECT_EQ(active_b_, 1);
}
TEST_F(
    SceneTransferTest, UnknownUncommittedCasRetriesExactWriteWithoutSkipping) {
    enter_a();
    store_.uncertain_before_commit = true;
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(12))->code(), 3105);
    transfer(3, 2, 13);
    wait([&] {
        return reply(13).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(13))->code(), 3105);
    transfer(2, 2, 14);
    wait([&] {
        return reply(14).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(14)));
    EXPECT_EQ(store_.commits, 2);
    EXPECT_EQ(store_.scene_state({7001, 1, 10})->last_result->command.seq, 2);
}
TEST_F(
    SceneTransferTest,
    LostActivationNeverRollsBackAndSameSequenceRestoresTarget) {
    enter_a();
    drop_activation_b_ = true;
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    EXPECT_EQ(common::decode_edge_error(*reply(12))->code(), 3105);
    EXPECT_EQ(
        store_.scene_state({7001, 1, 10})->confirmed_location->shard_id, 2);
    drop_activation_b_ = false;
    transfer(2, 2, 14);
    wait([&] {
        return reply(14).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(14)));
    EXPECT_EQ(store_.commits, 2);
    EXPECT_EQ(active_b_, 1);
}
TEST_F(
    SceneTransferTest, LostSourceRemovalCannotBlockTargetMovementOrRollback) {
    enter_a();
    drop_remove_ = true;
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(12)));
    wait([&] {
        return removals_completed_.load() > 0;
    });
    move({1}, 3, 13);
    wait(
        [&] {
            return reply(13).has_value();
        },
        2s);
    ASSERT_TRUE(common::decode_scene_command_result(*reply(13)));
    EXPECT_EQ(
        store_.scene_state({7001, 1, 10})->confirmed_location->shard_id, 2);
}
TEST_F(SceneTransferTest, HungTargetDoesNotBlockOtherCharacterOnSource) {
    enter_a();
    scenes_->selected({2}, {7002, 1, 20});
    enter({2}, 1, 21);
    wait([&] {
        return reply(21).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(21)));
    drop_prepare_b_ = true;
    transfer(2, 2, 12);
    wait([&] {
        return prepares_b_.load() > 0;
    });
    move({2}, 2, 22);
    wait(
        [&] {
            return reply(22).has_value();
        },
        2s);
    ASSERT_TRUE(common::decode_scene_command_result(*reply(22)));
    EXPECT_FALSE(reply(12));
}
TEST_F(
    SceneTransferTest,
    SameInstanceRestartNeedsNewEpochAndPreservesLastLocation) {
    enter_a();
    transfer(2, 2, 12);
    wait([&] {
        return reply(12).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(12)));
    const auto old = store_.scene_state({7001, 1, 10});
    stop_b();
    start_b("scene-boot-b-new");
    common::GetSceneState query;
    ASSERT_TRUE(scenes_->handle({1}, common::encode(query, 13)));
    wait([&] {
        return reply(13).has_value();
    });
    enter({1}, 3, 14);
    wait([&] {
        return reply(14).has_value();
    });
    ASSERT_TRUE(common::decode_scene_command_result(*reply(14)));
    const auto value = store_.scene_state({7001, 1, 10});
    EXPECT_GT(value->binding_epoch, old->binding_epoch);
    EXPECT_EQ(value->confirmed_location, old->confirmed_location);
    EXPECT_EQ(value->owner->scene_incarnation, "scene-boot-b-new");
}
}  // namespace
}  // namespace realm::game::realm
