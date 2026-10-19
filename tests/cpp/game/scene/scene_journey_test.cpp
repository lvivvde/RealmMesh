#include <atomic>
#include <future>
#include <map>
#include "../realm/journey_fixture.hpp"
#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/game/client/scene_session.hpp"
#include "realmmesh/game/common/scene_store.hpp"
#include "realmmesh/game/scene/scene_shard.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"

namespace realm::game::realm {
namespace {
constexpr std::uint64_t second_account_id = 7002;
class SceneJourneyTest : public RealmJourneyTest {
protected:
    void prepare_services(const std::string& database) override {
        database_ = database;
        const auto tls =
            std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY);
        set_environment("REALMMESH_SERVICE_CA_FILE", (tls / "ca.pem").string());
        set_environment(
            "REALMMESH_SCENE_CERTIFICATE_FILE", (tls / "scene.pem").string());
        set_environment(
            "REALMMESH_SCENE_PRIVATE_KEY_FILE", (tls / "scene.key").string());
        set_environment(
            "REALMMESH_REALM_CERTIFICATE_FILE", (tls / "realm.pem").string());
        set_environment(
            "REALMMESH_REALM_PRIVATE_KEY_FILE", (tls / "realm.key").string());
        scene_port_ = test_support::unused_loopback_ports(1).front();
        copy_configs(
            scene_root_.path(),
            "return { services = { { name = \"scene\" } } }");
        rewrite(
            scene_root_.path() / "services/scene.lua",
            "listen_port = 7200",
            "listen_port = " + std::to_string(scene_port_));
        rewrite(
            scene_root_.path() / "services/scene.lua",
            "metrics_port = 9106",
            "metrics_port = 0");
        rewrite(
            realm_root_.path() / "services/realm.lua",
            "enabled = false",
            "enabled = true");
        rewrite(
            realm_root_.path() / "services/realm.lua",
            "port = 7200",
            "port = " + std::to_string(scene_port_));
        // 静态后备端口故意不可用,真实 Scene 地址只能来自 etcd 发现。
        rewrite(
            realm_root_.path() / "services/realm.lua",
            "port = " + std::to_string(scene_port_),
            "port = 1");
        for (const auto& root : {realm_root_.path(), scene_root_.path()}) {
            rewrite(
                root / "common/discovery.lua",
                "enabled = false",
                "enabled = true");
        }
        rewrite(
            scene_root_.path() / "common/discovery.lua",
            "http://127.0.0.1:2379",
            etcd_.endpoint());
        common::MongoPlayerDataStore provisioning(
            test_support::MongodProcess::shared().uri(), database);
        provisioning.provision_account(
            {.account_id = second_account_id,
             .account_name = "scene-second",
             .credential = "second-secret",
             .banned = false,
             .whitelisted = true});
        scene_.emplace(scene_root_.path());
        wait_for_port(scene_port_, "scene");
    }
    void TearDown() override {
        RealmJourneyTest::TearDown();
        scene_.reset();
        if (const char* directory =
                std::getenv("REALMMESH_SCENE_ARTIFACT_DIRECTORY")) {
            const auto case_name =
                ::testing::UnitTest::GetInstance()->current_test_info()->name();
            const auto target = std::filesystem::path(directory) / case_name;
            std::filesystem::create_directories(target);
            for (const auto& [name, root] :
                 std::vector<std::pair<std::string, std::filesystem::path>>{
                     {"entry", mesh_root_.path()},
                     {"realm", realm_root_.path()},
                     {"scene", scene_root_.path()}}) {
                if (std::filesystem::exists(root / "logs"))
                    std::filesystem::copy(
                        root / "logs",
                        target / name,
                        std::filesystem::copy_options::recursive |
                            std::filesystem::copy_options::overwrite_existing);
            }
        }
        for (const auto* name :
             {"REALMMESH_SERVICE_CA_FILE",
              "REALMMESH_SCENE_CERTIFICATE_FILE",
              "REALMMESH_SCENE_PRIVATE_KEY_FILE",
              "REALMMESH_REALM_CERTIFICATE_FILE",
              "REALMMESH_REALM_PRIVATE_KEY_FILE"})
            ::unsetenv(name);
    }
    static std::map<std::uint64_t, common::SceneLocation> positions(
        const common::SceneState& state) {
        std::map<std::uint64_t, common::SceneLocation> result;
        for (const auto& member : state.snapshot().members()) {
            result[member.character_id()] = {
                member.location().shard_id(),
                member.location().x(),
                member.location().y()};
        }
        return result;
    }
    static common::SceneEvent event(
        client::SceneSession& scene,
        common::SceneEvent::Kind kind,
        std::uint64_t character) {
        const auto deadline = soon();
        while (client::Clock::now() < deadline) {
            const auto notice = scene.poll_scene(deadline);
            if (!notice) break;
            if (const auto* value = std::get_if<common::SceneEvent>(&*notice);
                value && value->kind() == kind &&
                value->member().character_id() == character)
                return *value;
        }
        ADD_FAILURE() << "missing Scene event " << kind << " for " << character;
        return {};
    }
    test_support::TemporaryDirectory scene_root_{"scene-journey-"};
    std::optional<MeshProcess> scene_;
    std::uint16_t scene_port_{0};
    std::string database_;
};

TEST_F(
    SceneJourneyTest, TwoRealLoginsObserveTwentyConfirmedMovesReplayAndLeave) {
    auto a = login();
    auto b = login("scene-second", "second-secret");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_NE(a->scene(), nullptr);
    ASSERT_NE(b->scene(), nullptr);
    EXPECT_EQ(a->scene()->enter_scene(1, soon()).error_code, 3003);
    const auto hero_a = a->create_character("SceneAlpha", soon());
    const auto hero_b = b->create_character("SceneBeta", soon());
    ASSERT_TRUE(hero_a.ok());
    ASSERT_TRUE(hero_b.ok());
    const auto ca = hero_a.value.character_id, cb = hero_b.value.character_id;
    ASSERT_TRUE(a->select_character(ca, soon()).ok());
    ASSERT_TRUE(b->select_character(cb, soon()).ok());
    const auto joined_a = a->scene()->enter_scene(1, soon());
    ASSERT_TRUE(joined_a.ok()) << joined_a.error_code;
    ASSERT_TRUE(b->scene()->enter_scene(1, soon()).ok());
    const auto join = event(*a->scene(), common::SceneEvent::KIND_JOIN, cb);
    EXPECT_GT(join.event_seq(), 0);
    const auto initial_a = a->scene()->get_scene_state(soon());
    const auto initial_b = b->scene()->get_scene_state(soon());
    ASSERT_TRUE(initial_a.ok());
    ASSERT_TRUE(initial_b.ok());
    ASSERT_EQ(initial_a.value.snapshot().members_size(), 2);
    EXPECT_EQ(positions(initial_a.value), positions(initial_b.value));
    EXPECT_EQ(
        initial_a.value.snapshot().event_seq(),
        initial_b.value.snapshot().event_seq());
    std::uint64_t event_seq = initial_a.value.snapshot().event_seq();
    for (std::uint64_t i = 1; i <= 10; ++i) {
        const auto moved_a = a->scene()->move_in_scene(
            i + 1, static_cast<int>(i * 10), 0, soon());
        ASSERT_TRUE(moved_a.ok()) << moved_a.error_code;
        const auto seen_b =
            event(*b->scene(), common::SceneEvent::KIND_MOVE, ca);
        const auto seen_a =
            event(*a->scene(), common::SceneEvent::KIND_MOVE, ca);
        EXPECT_EQ(seen_a.SerializeAsString(), seen_b.SerializeAsString());
        EXPECT_EQ(seen_a.event_seq(), ++event_seq);
        EXPECT_EQ(seen_a.member().location().x(), static_cast<int>(i * 10));
        const auto moved_b = b->scene()->move_in_scene(
            i + 1, 0, static_cast<int>(i * 10), soon());
        ASSERT_TRUE(moved_b.ok()) << moved_b.error_code;
        const auto a_sees_b =
            event(*a->scene(), common::SceneEvent::KIND_MOVE, cb);
        const auto b_sees_b =
            event(*b->scene(), common::SceneEvent::KIND_MOVE, cb);
        EXPECT_EQ(a_sees_b.SerializeAsString(), b_sees_b.SerializeAsString());
        EXPECT_EQ(a_sees_b.event_seq(), ++event_seq);
    }
    const auto replay = a->scene()->move_in_scene(11, 100, 0, soon());
    ASSERT_TRUE(replay.ok());
    EXPECT_TRUE(replay.value.replayed());
    EXPECT_EQ(a->scene()->move_in_scene(11, 101, 0, soon()).error_code, 3103);
    EXPECT_EQ(a->scene()->move_in_scene(13, 101, 0, soon()).error_code, 3103);
    EXPECT_EQ(a->scene()->move_in_scene(12, 1000, 0, soon()).error_code, 3102);
    ASSERT_TRUE(a->heartbeat(soon()));
    ASSERT_TRUE(a->train(1, soon()).ok());
    const auto final_a = a->scene()->get_scene_state(soon());
    const auto final_b = b->scene()->get_scene_state(soon());
    ASSERT_TRUE(final_a.ok());
    ASSERT_TRUE(final_b.ok());
    EXPECT_EQ(positions(final_a.value), positions(final_b.value));
    EXPECT_EQ(final_a.value.snapshot().event_seq(), event_seq);
    EXPECT_EQ(final_b.value.snapshot().event_seq(), event_seq);
    common::MongoPlayerDataStore database(
        test_support::MongodProcess::shared().uri(), database_);
    const auto durable_a = database.scene_state({journey_account_id, 1, ca});
    const auto durable_b = database.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(durable_a);
    ASSERT_TRUE(durable_b);
    EXPECT_EQ(durable_a->last_result->command.seq, 11);
    EXPECT_EQ(durable_b->last_result->command.seq, 11);
    EXPECT_EQ(
        durable_a->confirmed_location,
        std::optional(common::SceneLocation{1, 100, 0}));
    EXPECT_EQ(
        durable_b->confirmed_location,
        std::optional(common::SceneLocation{1, 0, 100}));
    EXPECT_EQ(positions(final_a.value).at(ca), *durable_a->confirmed_location);
    EXPECT_EQ(positions(final_a.value).at(cb), *durable_b->confirmed_location);
    RecordProperty(
        "database_verification",
        "two characters; seq=11; positions=(100,0),(0,100); snapshots and DB agree");
    b->close();
    const auto leave = event(*a->scene(), common::SceneEvent::KIND_LEAVE, cb);
    EXPECT_EQ(leave.event_seq(), event_seq + 1);
    const auto after = a->scene()->get_scene_state(soon());
    ASSERT_TRUE(after.ok());
    EXPECT_EQ(after.value.snapshot().members_size(), 1);
}

TEST_F(
    SceneJourneyTest,
    MissingScenePreservesHeartbeatTrainingAndDurableLocation) {
    auto a = login();
    ASSERT_NE(a, nullptr);
    const auto hero = a->create_character("SceneAbsent", soon());
    ASSERT_TRUE(hero.ok());
    ASSERT_TRUE(a->select_character(hero.value.character_id, soon()).ok());
    ASSERT_TRUE(a->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(a->scene()->move_in_scene(2, 10, 0, soon()).ok());
    scene_.reset();
    ASSERT_TRUE(a->heartbeat(soon()));
    ASSERT_TRUE(a->train(1, soon()).ok());
    const auto denied = a->scene()->move_in_scene(3, 20, 0, soon());
    EXPECT_FALSE(denied.ok());
    ASSERT_TRUE(a->heartbeat(soon()));
    ASSERT_TRUE(a->train(2, soon()).ok());
    const auto state = a->scene()->get_scene_state(soon());
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state.value.last_scene_seq(), 2);
    EXPECT_EQ(state.value.confirmed_location().x(), 10);
    EXPECT_FALSE(state.value.has_snapshot());
    EXPECT_EQ(
        state.value.phase(),
        ::realmmesh::protocol::scene::v1::PHASE_UNAVAILABLE);
    // Scene 仍缺失时重启 Realm:不能等待 Scene 就绪才开放既有业务。
    restart_realm();
    auto again = login();
    ASSERT_NE(again, nullptr);
    ASSERT_TRUE(again->select_character(hero.value.character_id, soon()).ok());
    EXPECT_EQ(again->scene()->enter_scene(3, soon()).error_code, 3104);
    ASSERT_TRUE(again->heartbeat(soon()));
    ASSERT_TRUE(again->train(3, soon()).ok());
    const auto restored = again->scene()->get_scene_state(soon());
    ASSERT_TRUE(restored.ok());
    EXPECT_EQ(restored.value.last_scene_seq(), 2);
    EXPECT_EQ(restored.value.confirmed_location().x(), 10);
}
class SceneTransferJourneyTest : public SceneJourneyTest {
protected:
    void prepare_services(const std::string& database) override {
        SceneJourneyTest::prepare_services(database);
        const auto tls =
            std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY);
        set_environment(
            "REALMMESH_SCENE2_CERTIFICATE_FILE", (tls / "scene2.pem").string());
        set_environment(
            "REALMMESH_SCENE2_PRIVATE_KEY_FILE", (tls / "scene2.key").string());
        port_b_ = test_support::unused_loopback_ports(1).front();
        copy_configs(
            root_b_.path(), "return { services = { { name = \"scene\" } } }");
        const auto config = root_b_.path() / "services/scene.lua";
        rewrite(config, "scene-dev-01", "scene-dev-02");
        rewrite(config, "shard_id = 1", "shard_id = 2");
        rewrite(
            config,
            "listen_port = 7200",
            "listen_port = " + std::to_string(port_b_));
        rewrite(config, "metrics_port = 9106", "metrics_port = 0");
        rewrite(
            config,
            "REALMMESH_SCENE_CERTIFICATE_FILE",
            "REALMMESH_SCENE2_CERTIFICATE_FILE");
        rewrite(
            config,
            "REALMMESH_SCENE_PRIVATE_KEY_FILE",
            "REALMMESH_SCENE2_PRIVATE_KEY_FILE");
        rewrite(
            root_b_.path() / "common/discovery.lua",
            "enabled = false",
            "enabled = true");
        rewrite(
            root_b_.path() / "common/discovery.lua",
            "http://127.0.0.1:2379",
            etcd_.endpoint());
        rewrite(
            realm_root_.path() / "services/realm.lua",
            "targets = {},",
            "targets = {{ shard_id = 2, host = \"127.0.0.1\", port = " +
                std::to_string(port_b_) +
                ", instance_id = \"scene-dev-02\", server_identity = \"scene-dev-02.scene.service\", entry_x = 200, entry_y = 300 }},");
        scene_b_.emplace(root_b_.path());
        wait_for_port(port_b_, "scene-b");
    }
    void TearDown() override {
        SceneJourneyTest::TearDown();
        scene_b_.reset();
        if (const char* directory =
                std::getenv("REALMMESH_SCENE_ARTIFACT_DIRECTORY")) {
            const auto target = std::filesystem::path(directory) /
                                ::testing::UnitTest::GetInstance()
                                    ->current_test_info()
                                    ->name() /
                                "scene-b";
            if (std::filesystem::exists(root_b_.path() / "logs"))
                std::filesystem::copy(
                    root_b_.path() / "logs",
                    target,
                    std::filesystem::copy_options::recursive |
                        std::filesystem::copy_options::overwrite_existing);
        }
        ::unsetenv("REALMMESH_SCENE2_CERTIFICATE_FILE");
        ::unsetenv("REALMMESH_SCENE2_PRIVATE_KEY_FILE");
    }
    std::unique_ptr<client::RealmSession> selected(
        std::uint64_t& character,
        std::string_view account = journey_account,
        std::string_view credential = journey_credential) {
        auto session = login(account, credential);
        if (!session) return {};
        const auto hero = session->create_character(
            account == journey_account ? "TransferAlpha" : "TransferBeta",
            soon());
        if (!hero.ok()) {
            ADD_FAILURE() << hero.error_code;
            return {};
        }
        character = hero.value.character_id;
        if (!session->select_character(character, soon()).ok()) {
            ADD_FAILURE();
            return {};
        }
        return session;
    }
    void wait_instance_expired(
        const std::string& type, const std::string& instance) {
        cluster::EtcdServiceRegistry registry(
            {.endpoint = etcd_.endpoint(), .background_maintenance = false});
        const auto deadline = client::Clock::now() + 12s;
        while (client::Clock::now() < deadline) {
            bool found = false;
            for (const auto& value : registry.discover(type))
                if (value.instance_id == instance) found = true;
            if (!found && registry.last_error().empty()) return;
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("old registration lease did not expire");
    }
    void stale_write_rejected(
        const common::SceneStateRecord& old,
        std::uint64_t seq,
        const common::SceneLocation& expected) {
        common::MongoPlayerDataStore database(
            test_support::MongodProcess::shared().uri(), database_);
        const auto prior = database.scene_state(old.character);
        common::SceneWrite stale{
            old.character,
            old.binding_epoch,
            old.owner,
            *old.owner,
            {common::SceneCommandType::Move,
             seq,
             old.confirmed_location->x + 10,
             old.confirmed_location->y},
            {old.confirmed_location->shard_id,
             old.confirmed_location->x + 10,
             old.confirmed_location->y}};
        EXPECT_EQ(
            database.commit_scene(stale).outcome,
            common::SceneWriteOutcome::StaleBinding);
        const auto after = database.scene_state(old.character);
        ASSERT_TRUE(after);
        EXPECT_EQ(after, prior);
        EXPECT_EQ(after->confirmed_location, std::optional(expected));
    }
    test_support::TemporaryDirectory root_b_{"scene-transfer-b-"};
    std::optional<MeshProcess> scene_b_;
    std::uint16_t port_b_{0};
};
TEST_F(
    SceneTransferJourneyTest, TwoInstancesTransferReplayLeaveAndDatabaseAgree) {
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(a->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(b->scene()->enter_scene(1, soon()).ok());
    EXPECT_EQ(a->scene()->transfer_scene(2, 99, soon()).error_code, 3102);
    EXPECT_EQ(a->scene()->transfer_scene(2, 1, soon()).error_code, 3102);
    const auto moved = a->scene()->transfer_scene(2, 2, soon());
    ASSERT_TRUE(moved.ok()) << moved.error_code;
    EXPECT_EQ(moved.value.location().shard_id(), 2);
    EXPECT_EQ(moved.value.location().x(), 200);
    EXPECT_EQ(moved.value.location().y(), 300);
    static_cast<void>(event(*b->scene(), common::SceneEvent::KIND_LEAVE, ca));
    const auto before_replay = a->scene()->get_scene_state(soon());
    ASSERT_TRUE(before_replay.ok());
    ASSERT_EQ(before_replay.value.snapshot().members_size(), 1);
    const auto replay = a->scene()->transfer_scene(2, 2, soon());
    ASSERT_TRUE(replay.ok());
    EXPECT_TRUE(replay.value.replayed());
    const auto after_replay = a->scene()->get_scene_state(soon());
    ASSERT_TRUE(after_replay.ok());
    EXPECT_EQ(
        after_replay.value.snapshot().event_seq(),
        before_replay.value.snapshot().event_seq());
    const auto source = b->scene()->get_scene_state(soon());
    ASSERT_TRUE(source.ok());
    ASSERT_EQ(source.value.snapshot().members_size(), 1);
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    static_cast<void>(event(*a->scene(), common::SceneEvent::KIND_JOIN, cb));
    ASSERT_TRUE(a->scene()->move_in_scene(3, 210, 300, soon()).ok());
    EXPECT_EQ(
        event(*b->scene(), common::SceneEvent::KIND_MOVE, ca)
            .member()
            .location()
            .x(),
        210);
    common::MongoPlayerDataStore database(
        test_support::MongodProcess::shared().uri(), database_);
    const auto old = database.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(old);
    ASSERT_TRUE(old->owner);
    ASSERT_TRUE(a->scene()->transfer_scene(4, 1, soon()).ok());
    static_cast<void>(event(*b->scene(), common::SceneEvent::KIND_LEAVE, ca));
    const auto state_a = a->scene()->get_scene_state(soon());
    const auto state_b = b->scene()->get_scene_state(soon());
    ASSERT_TRUE(state_a.ok());
    ASSERT_TRUE(state_b.ok());
    EXPECT_EQ(state_a.value.snapshot().shard_id(), 1);
    EXPECT_EQ(state_b.value.snapshot().shard_id(), 2);
    ASSERT_EQ(state_a.value.snapshot().members_size(), 1);
    ASSERT_EQ(state_b.value.snapshot().members_size(), 1);
    const auto durable_a = database.scene_state({journey_account_id, 1, ca});
    const auto durable_b = database.scene_state({second_account_id, 1, cb});
    EXPECT_EQ(durable_a->last_result->command.seq, 4);
    EXPECT_EQ(durable_b->last_result->command.seq, 2);
    EXPECT_EQ(durable_a->owner->scene_instance_id, "scene-dev-01");
    EXPECT_EQ(durable_b->owner->scene_instance_id, "scene-dev-02");
    stale_write_rejected(*old, 5, {1, 0, 0});
    RecordProperty(
        "database_verification",
        "A->B->A; independent scenes; old epoch CAS rejected; confirmed positions match snapshots");
}
TEST_F(
    SceneTransferJourneyTest,
    SceneSigkillKeepsOtherShardMovingAndNewIncarnationRestoresLocation) {
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    ASSERT_TRUE(a->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(b->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    common::MongoPlayerDataStore database(
        test_support::MongodProcess::shared().uri(), database_);
    const auto old = database.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(old);
    ASSERT_TRUE(old->owner);
    scene_b_->crash();
    ASSERT_TRUE(a->scene()->move_in_scene(2, 10, 0, soon()).ok());
    ASSERT_TRUE(a->heartbeat(soon()));
    ASSERT_TRUE(a->train(1, soon()).ok());
    const auto missing = b->scene()->get_scene_state(soon());
    ASSERT_TRUE(missing.ok());
    EXPECT_EQ(missing.value.last_scene_seq(), 2);
    EXPECT_EQ(missing.value.confirmed_location().shard_id(), 2);
    EXPECT_FALSE(missing.value.has_snapshot());
    wait_instance_expired("scene", "scene-dev-02");
    scene_b_.reset();
    scene_b_.emplace(root_b_.path());
    wait_for_port(port_b_, "scene-b restart");
    ASSERT_TRUE(b->scene()->enter_scene(3, soon()).ok());
    const auto recovered = database.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(recovered);
    ASSERT_TRUE(recovered->owner);
    EXPECT_NE(
        recovered->owner->scene_incarnation, old->owner->scene_incarnation);
    EXPECT_GT(recovered->binding_epoch, old->binding_epoch);
    EXPECT_EQ(recovered->confirmed_location, old->confirmed_location);
    ASSERT_TRUE(b->scene()->move_in_scene(4, 210, 300, soon()).ok());
    stale_write_rejected(*old, 5, {2, 210, 300});
}

// 只在切换故障用例中替代 B 的 IO：生产 SceneShard + mTLS，暂停激活确认。
class PausedActivationScene {
public:
    explicit PausedActivationScene(std::uint16_t port) {
        const auto tls =
            std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY);
        network::TransportConfig config;
        config.name = "paused-b";
        config.listen_address = "127.0.0.1";
        config.listen_port = port;
        config.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = (tls / "scene2.pem").string(),
            .private_key_file = (tls / "scene2.key").string(),
            .alpn = common::scene_internal_alpn,
            .client_ca_file = (tls / "ca.pem").string(),
            .allowed_client_identities = {"realm-dev-01.realm.service"}};
        auto transports =
            network::TransportFactory::create_enabled(std::vector{config});
        server_ = std::move(transports.front());
        thread_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested())
                for (const auto& event : server_->poll_once(1ms)) {
                    if (event.kind ==
                        network::TransportEventKind::SessionClosed)
                        shard_.closed(event.session_id);
                    if (event.kind !=
                        network::TransportEventKind::MessageReceived)
                        continue;
                    const auto request =
                        common::decode_scene_request(event.payload);
                    if (!request) continue;
                    if (request->operation() ==
                        ::realmmesh::protocol::scene_internal::v1::
                            OPERATION_ACTIVATE) {
                        activation_seen = true;
                        continue;
                    }
                    static_cast<void>(server_->send(
                        event.session_id,
                        common::encode(
                            shard_.handle(event.session_id, *request),
                            *common::scene_internal_request_id(
                                event.payload))));
                    static_cast<void>(shard_.drain_events());
                }
        });
    }
    ~PausedActivationScene() {
        thread_.request_stop();
        thread_.join();
    }
    std::atomic_bool activation_seen{false};

private:
    std::unique_ptr<network::IMessageTransport> server_;
    scene::SceneShard shard_{"scene-dev-02", "paused-b-incarnation", 2};
    std::jthread thread_;
};
TEST_F(
    SceneTransferJourneyTest,
    RealmSigkillAfterTargetCommitRecoversWithoutRollingBackSource) {
    std::uint64_t character = 0;
    auto first = selected(character);
    ASSERT_NE(first, nullptr);
    ASSERT_TRUE(first->scene()->enter_scene(1, soon()).ok());
    common::MongoPlayerDataStore database(
        test_support::MongodProcess::shared().uri(), database_);
    // 让 B 的准备成功、激活停住；数据库已经越过唯一切换点。
    scene_b_.reset();
    auto paused = std::make_unique<PausedActivationScene>(port_b_);
    auto transfer = std::async(std::launch::async, [&] {
        return first->scene()->transfer_scene(2, 2, soon());
    });
    const auto deadline = client::Clock::now() + 5s;
    while (!paused->activation_seen && client::Clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(paused->activation_seen);
    const auto old = database.scene_state({journey_account_id, 1, character});
    ASSERT_TRUE(old);
    ASSERT_TRUE(old->owner);
    ASSERT_EQ(old->last_result->command.seq, 2);
    ASSERT_EQ(old->confirmed_location->shard_id, 2);
    realm_->crash();
    static_cast<void>(transfer.get());
    first->close();
    paused.reset();
    scene_b_.emplace(root_b_.path());
    wait_for_port(port_b_, "real scene-b restart");
    wait_instance_expired("realm", "realm-dev-01");
    realm_.reset();
    start_realm();
    auto again = login();
    ASSERT_NE(again, nullptr);
    ASSERT_TRUE(again->select_character(character, soon()).ok());
    const auto state = again->scene()->get_scene_state(soon());
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state.value.last_scene_seq(), 2);
    EXPECT_EQ(state.value.confirmed_location().shard_id(), 2);
    EXPECT_EQ(state.value.confirmed_location().x(), 200);
    EXPECT_EQ(state.value.confirmed_location().y(), 300);
    const auto replay = again->scene()->transfer_scene(2, 2, soon());
    ASSERT_TRUE(replay.ok());
    EXPECT_TRUE(replay.value.replayed());
    EXPECT_EQ(
        database.scene_state({journey_account_id, 1, character})->binding_epoch,
        old->binding_epoch);
    ASSERT_TRUE(again->scene()->enter_scene(3, soon()).ok());
    const auto recovered =
        database.scene_state({journey_account_id, 1, character});
    ASSERT_TRUE(recovered);
    ASSERT_TRUE(recovered->owner);
    EXPECT_NE(
        recovered->owner->realm_incarnation, old->owner->realm_incarnation);
    EXPECT_GT(recovered->binding_epoch, old->binding_epoch);
    EXPECT_EQ(recovered->confirmed_location, old->confirmed_location);
    stale_write_rejected(*old, 4, {2, 200, 300});
    ASSERT_TRUE(again->heartbeat(soon()));
    ASSERT_TRUE(again->train(1, soon()).ok());
}
}  // namespace
}  // namespace realm::game::realm
