#include <fcntl.h>
#include <httplib.h>
#include <array>
#include <atomic>
#include <future>
#include <map>
#include <vector>
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
    virtual bool dynamic_scene() const { return false; }
    void configure_dynamic_scene(
        const std::filesystem::path& root, const std::string& database) {
        rewrite(
            root / "services/scene.lua", "dynamic = false", "dynamic = true");
        rewrite(
            root / "common/discovery.lua",
            "required = false",
            "required = true");
        const auto metrics_port =
            test_support::unused_loopback_ports(1).front();
        scene_metrics_ports_[root] = metrics_port;
        rewrite(
            root / "services/scene.lua",
            "metrics_port = 0",
            "metrics_port = " + std::to_string(metrics_port));
        test_support::point_player_data_at(
            root, test_support::MongodProcess::shared(), database);
    }
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
        if (dynamic_scene()) {
            configure_dynamic_scene(scene_root_.path(), database);
            rewrite(
                realm_root_.path() / "services/realm.lua",
                "dynamic = false",
                "dynamic = true");
            rewrite(
                realm_root_.path() / "common/discovery.lua",
                "required = false",
                "required = true");
            realm_metrics_port_ =
                test_support::unused_loopback_ports(1).front();
            rewrite(
                realm_root_.path() / "services/realm.lua",
                "metrics_port = 0",
                "metrics_port = " + std::to_string(realm_metrics_port_));
        }
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
    std::map<std::filesystem::path, std::uint16_t> scene_metrics_ports_;
    std::uint16_t realm_metrics_port_{0};
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
    virtual bool start_second_scene() const { return true; }
    void start_b() {
        scene_b_.emplace(root_b_.path());
        wait_for_port(port_b_, "scene-b");
    }
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
        if (dynamic_scene()) configure_dynamic_scene(root_b_.path(), database);
        if (start_second_scene()) start_b();
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
class SceneControlProcess final {
public:
    SceneControlProcess(
        std::vector<std::string> arguments, std::filesystem::path output)
        : output_(std::move(output)) {
        arguments.insert(arguments.begin(), REALMMESH_SCENE_CONTROL_EXECUTABLE);
        std::vector<char*> argv;
        for (auto& argument : arguments) argv.push_back(argument.data());
        argv.push_back(nullptr);
        pid_ = ::fork();
        if (pid_ < 0) throw std::runtime_error("control fork failed");
        if (pid_ == 0) {
            const int fd =
                ::open(output_.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
            if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0 ||
                ::dup2(fd, STDERR_FILENO) < 0)
                _exit(126);
            ::close(fd);
            ::execv(REALMMESH_SCENE_CONTROL_EXECUTABLE, argv.data());
            _exit(127);
        }
    }
    ~SceneControlProcess() {
        if (pid_ > 0) {
            static_cast<void>(::kill(pid_, SIGKILL));
            int status = 0;
            static_cast<void>(::waitpid(pid_, &status, 0));
        }
    }
    int wait(std::chrono::seconds timeout = 30s) {
        const auto deadline = client::Clock::now() + timeout;
        while (!result_ && client::Clock::now() < deadline) {
            int status = 0;
            const auto finished = ::waitpid(pid_, &status, WNOHANG);
            if (finished == pid_) {
                pid_ = -1;
                result_ = WIFEXITED(status) ? WEXITSTATUS(status)
                                            : 128 + WTERMSIG(status);
            } else
                std::this_thread::sleep_for(5ms);
        }
        if (!result_)
            throw std::runtime_error("Scene control process did not finish");
        return *result_;
    }
    nlohmann::json last() const {
        std::ifstream input(output_);
        nlohmann::json last = nlohmann::json::object();
        std::string line;
        while (std::getline(input, line)) {
            auto value = nlohmann::json::parse(line, nullptr, false);
            if (value.is_object()) last = std::move(value);
        }
        return last;
    }
    std::string output() const { return read_file(output_); }

private:
    pid_t pid_{-1};
    std::optional<int> result_;
    std::filesystem::path output_;
};
class SceneDynamicJourneyTest : public SceneTransferJourneyTest {
protected:
    bool dynamic_scene() const override { return true; }
    common::MongoPlayerDataStore database() const {
        common::MongoPlayerDataOptions options;
        options.credential_hash_cost = common::CredentialHashCost::Minimum;
        return {
            test_support::MongodProcess::shared().uri(), database_, options};
    }
    common::SceneShardRecord wait_shard(
        std::uint64_t id, const std::string& instance) {
        auto store = database();
        const auto deadline = client::Clock::now() + 10s;
        while (client::Clock::now() < deadline) {
            const auto state = store.shard_state(1, id);
            if (state && state->owner && state->owner->instance_id == instance)
                return *state;
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error("dynamic shard owner did not become ready");
    }
    std::unique_ptr<client::RealmSession> isolated_login(
        const std::string& account) {
        client::WireEndpoints endpoints;
        endpoints.login_verify_port = ports_.login_verify;
        endpoints.queue_port = ports_.queue;
        endpoints.verify_peer = false;
        client::WireEnterRealmRedeemer redeemer;
        client::WireLoginTransport transport(
            endpoints, redeemer, client::WireTransportOptions{});
        return login(account, "load-secret", &transport);
    }
    std::string metrics(std::uint16_t port) {
        httplib::Client client("127.0.0.1", port);
        client.set_connection_timeout(1, 0);
        client.set_read_timeout(1, 0);
        const auto response = client.Get("/metrics");
        if (!response || response->status != 200) {
            ADD_FAILURE() << "metrics endpoint unavailable";
            return {};
        }
        return response->body;
    }
    std::unique_ptr<SceneControlProcess> control(
        bool second,
        bool drain,
        const std::string& operation = "test-drain",
        int timeout = 20000,
        std::vector<std::string> extra = {}) {
        std::vector<std::string> args{
            drain ? "--drain" : "--status",
            "--host",
            "127.0.0.1",
            "--port",
            std::to_string(second ? port_b_ : scene_port_),
            "--instance",
            second ? "scene-dev-02" : "scene-dev-01",
            "--server-identity",
            second ? "scene-dev-02.scene.service"
                   : "scene-dev-01.scene.service",
            "--operation-id",
            operation,
            "--timeout-ms",
            std::to_string(timeout)};
        args.insert(args.end(), extra.begin(), extra.end());
        const auto path =
            scene_root_.path() / "logs" /
            ("control-" + std::to_string(next_control_++) + ".log");
        std::filesystem::create_directories(path.parent_path());
        return std::make_unique<SceneControlProcess>(std::move(args), path);
    }
    std::size_t next_control_{0};
};
class SceneDynamicJoinJourneyTest : public SceneDynamicJourneyTest {
protected:
    bool start_second_scene() const override { return false; }
};
TEST_F(
    SceneDynamicJoinJourneyTest,
    RuntimeJoinAllocatesNewShardWithoutRebalancing) {
    const auto first = wait_shard(1, "scene-dev-01");
    auto store = database();
    EXPECT_FALSE(store.shard_state(1, 2));
    std::uint64_t character = 0;
    auto player = selected(character);
    ASSERT_TRUE(player);
    ASSERT_TRUE(player->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(player->scene()->move_in_scene(2, 30, 0, soon()).ok());
    const auto before = store.scene_state({journey_account_id, 1, character});
    ASSERT_TRUE(before && before->owner);
    start_b();
    const auto second = wait_shard(2, "scene-dev-02");
    EXPECT_EQ(store.shard_state(1, 1), std::optional(first));
    EXPECT_EQ(second.version, 1);
    ASSERT_TRUE(player->scene()->move_in_scene(3, 40, 0, soon()).ok());
    const auto old = store.scene_state({journey_account_id, 1, character});
    ASSERT_TRUE(player->scene()->transfer_scene(4, 2, soon()).ok());
    const auto after = store.scene_state({journey_account_id, 1, character});
    ASSERT_TRUE(after && after->owner);
    EXPECT_EQ(after->owner->scene_instance_id, "scene-dev-02");
    EXPECT_EQ(after->owner->shard_version, second.version);
    EXPECT_EQ(
        after->confirmed_location,
        std::optional(common::SceneLocation{2, 200, 300}));
    stale_write_rejected(*old, 4, {2, 200, 300});
    ASSERT_TRUE(player->heartbeat(soon()));
    ASSERT_TRUE(player->train(1, soon()).ok());
    RecordProperty(
        "database_verification",
        "runtime_join: shard1 owner/version unchanged; shard2 allocated after real etcd+mTLS readiness; transfer seq4 and old CAS fenced");
}
TEST_F(
    SceneDynamicJourneyTest,
    DrainMovesMembersAndReleasesOnlyAfterConfirmedMigration) {
    wait_shard(1, "scene-dev-01");
    wait_shard(2, "scene-dev-02");
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(a->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(a->scene()->move_in_scene(2, 10, 0, soon()).ok());
    ASSERT_TRUE(b->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    auto store = database();
    const auto old = store.scene_state({journey_account_id, 1, ca});
    auto draining = control(false, true, "drain-success");
    ASSERT_TRUE(b->scene()->move_in_scene(3, 210, 300, soon()).ok());
    ASSERT_TRUE(b->heartbeat(soon()));
    ASSERT_TRUE(b->train(1, soon()).ok());
    ASSERT_EQ(draining->wait(), 0) << draining->output();
    EXPECT_EQ(draining->last().value("phase", "missing"), "complete");
    EXPECT_EQ(draining->last().value("owned_shards", -1), 0);
    EXPECT_EQ(draining->last().value("active_members", -1), 0);
    EXPECT_EQ(draining->last().value("prepared_members", -1), 0);
    const auto released = store.shard_state(1, 1);
    ASSERT_TRUE(released);
    EXPECT_FALSE(released->owner);
    EXPECT_EQ(released->version, 2);
    EXPECT_TRUE(store.scene_instance_state("scene-dev-01")->draining);
    const auto state = a->scene()->get_scene_state(soon());
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state.value.last_scene_seq(), 3);
    EXPECT_EQ(state.value.confirmed_location().shard_id(), 2);
    EXPECT_EQ(
        state.value.phase(), ::realmmesh::protocol::scene::v1::PHASE_ACTIVE);
    EXPECT_EQ(state.value.snapshot().members_size(), 2);
    ASSERT_TRUE(a->scene()->move_in_scene(4, 210, 300, soon()).ok());
    const auto before_repeat = store.scene_state({journey_account_id, 1, ca});
    auto repeated = control(false, true, "drain-success");
    ASSERT_EQ(repeated->wait(5s), 0) << repeated->output();
    EXPECT_EQ(store.scene_state({journey_account_id, 1, ca}), before_repeat);
    stale_write_rejected(*old, 3, {2, 210, 300});
    cluster::EtcdServiceRegistry registry(
        {.endpoint = etcd_.endpoint(), .background_maintenance = false});
    bool still_registered = false;
    for (const auto& instance : registry.discover("scene"))
        if (instance.instance_id == "scene-dev-01") still_registered = true;
    EXPECT_TRUE(still_registered);
    const auto scene_metrics =
        metrics(scene_metrics_ports_.at(scene_root_.path()));
    EXPECT_NE(scene_metrics.find("scene_owned_shards 0"), std::string::npos);
    EXPECT_NE(
        scene_metrics.find("scene_drain_remaining_members 0"),
        std::string::npos);
    EXPECT_NE(
        scene_metrics.find("scene_drain_attempts_total 1"), std::string::npos);
    const auto realm_metrics = metrics(realm_metrics_port_);
    EXPECT_NE(
        realm_metrics.find("scene_drain_transfers_total 1"), std::string::npos);
    EXPECT_NE(
        realm_metrics.find("scene_drain_pending_transfers 0"),
        std::string::npos);
    EXPECT_NE(realm_metrics.find("scene_ready_instances 1"), std::string::npos);
    EXPECT_NE(
        realm_metrics.find(
            "scene_shard_assignments_total{operation=\"assign\"} 2"),
        std::string::npos);
    EXPECT_NE(
        realm_metrics.find(
            "scene_shard_assignments_total{operation=\"release\"} 1"),
        std::string::npos);
    EXPECT_NE(
        realm_metrics.find("scene_shard_reconcile_errors_total 0"),
        std::string::npos);
    RecordProperty(
        "database_verification",
        "drain: transfer committed seq3; target movement seq4; source shard released at version2; repeated drain did not change character record; source registry retained");
}
TEST_F(
    SceneDynamicJourneyTest,
    FullTargetKeepsDrainingAndBothShardsMovableUntilExplicitFailure) {
    wait_shard(1, "scene-dev-01");
    wait_shard(2, "scene-dev-02");
    std::uint64_t character = 0;
    auto a = selected(character);
    ASSERT_TRUE(a);
    ASSERT_TRUE(a->scene()->enter_scene(1, soon()).ok());
    auto store = database();
    std::vector<std::unique_ptr<client::RealmSession>> occupants;
    for (std::uint64_t i = 0; i < 16; ++i) {
        const auto account = "drain-load-" + std::to_string(i);
        const auto id = 8000 + i;
        store.provision_account({id, account, "load-secret", false, true});
        store.provision_character(
            {9000 + i, id, 1, "DrainLoad" + std::to_string(i)});
    }
    // 仅并发准备真实登录，每个任务独占 transport/redeemer；选角、进入和
    // 转场仍逐个执行，完整保留每片16人的容量边界与真实后端断言。
    for (std::uint64_t offset = 0; offset < 16; offset += 4) {
        ASSERT_TRUE(a->heartbeat(soon()));
        for (const auto& occupant : occupants)
            ASSERT_TRUE(occupant->heartbeat(soon()));
        std::array<std::future<std::unique_ptr<client::RealmSession>>, 4>
            waiting;
        for (std::size_t i = 0; i < waiting.size(); ++i)
            waiting[i] =
                std::async(std::launch::async, [this, id = offset + i] {
                    return isolated_login("drain-load-" + std::to_string(id));
                });
        for (std::size_t i = 0; i < waiting.size(); ++i) {
            auto session = waiting[i].get();
            ASSERT_TRUE(session);
            ASSERT_TRUE(session->heartbeat(soon()));
            ASSERT_TRUE(
                session->select_character(9000 + offset + i, soon()).ok());
            ASSERT_TRUE(session->scene()->enter_scene(1, soon()).ok());
            ASSERT_TRUE(session->scene()->transfer_scene(2, 2, soon()).ok());
            occupants.push_back(std::move(session));
        }
    }
    auto draining = control(false, true, "full-target", 1200);
    ASSERT_TRUE(a->scene()->move_in_scene(2, 10, 0, soon()).ok());
    ASSERT_TRUE(
        occupants.front()->scene()->move_in_scene(3, 210, 300, soon()).ok());
    ASSERT_EQ(draining->wait(8s), 2) << draining->output();
    EXPECT_EQ(draining->last().value("phase", "missing"), "failed");
    EXPECT_EQ(draining->last().value("active_members", 0), 1);
    EXPECT_EQ(draining->last().value("owned_shards", 0), 1);
    const auto held = store.shard_state(1, 1);
    ASSERT_TRUE(held && held->owner);
    EXPECT_EQ(held->owner->instance_id, "scene-dev-01");
    EXPECT_TRUE(held->draining);
    const auto before = store.scene_state({journey_account_id, 1, character});
    ASSERT_TRUE(before && before->owner);
    EXPECT_EQ(before->last_result->command.seq, 2);
    EXPECT_EQ(
        before->confirmed_location,
        std::optional(common::SceneLocation{1, 10, 0}));
    auto repeat = control(false, true, "full-target", 60000);
    ASSERT_EQ(repeat->wait(5s), 2) << repeat->output();
    EXPECT_EQ(store.scene_state({journey_account_id, 1, character}), before);
    ASSERT_TRUE(a->scene()->move_in_scene(3, 20, 0, soon()).ok());
    ASSERT_TRUE(a->heartbeat(soon()));
    const auto scene_metrics =
        metrics(scene_metrics_ports_.at(scene_root_.path()));
    EXPECT_NE(scene_metrics.find("scene_drain_failed 1"), std::string::npos);
    EXPECT_NE(
        scene_metrics.find("scene_drain_remaining_members 1"),
        std::string::npos);
    EXPECT_NE(
        scene_metrics.find("scene_drain_failures_total{reason=\"timeout\"} 1"),
        std::string::npos);
    const auto realm_metrics = metrics(realm_metrics_port_);
    EXPECT_NE(
        realm_metrics.find("scene_drain_retries_total{reason=\"capacity\"}"),
        std::string::npos);
    RecordProperty(
        "database_verification",
        "drain_full: 16 real selected/logged-in characters retained on shard2; source shard1 remains owned+draining; source seq2 at(10,0), then seq3 at(20,0); repeated attempt did not extend or mutate receipt");
}
TEST_F(SceneDynamicJourneyTest, FailedCarrierRebuildsSameShardOnOtherInstance) {
    wait_shard(1, "scene-dev-01");
    wait_shard(2, "scene-dev-02");
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(a->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(a->scene()->move_in_scene(2, 50, 0, soon()).ok());
    ASSERT_TRUE(b->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    auto store = database();
    const auto old = store.scene_state({journey_account_id, 1, ca});
    scene_->crash();
    scene_.reset();
    ASSERT_TRUE(b->scene()->move_in_scene(3, 210, 300, soon()).ok());
    ASSERT_TRUE(b->heartbeat(soon()));
    ASSERT_TRUE(b->train(1, soon()).ok());
    wait_instance_expired("scene", "scene-dev-01");
    const auto recovered = wait_shard(1, "scene-dev-02");
    EXPECT_EQ(recovered.version, 2);
    const auto state = a->scene()->get_scene_state(soon());
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state.value.last_scene_seq(), 2);
    EXPECT_EQ(state.value.confirmed_location().shard_id(), 1);
    EXPECT_EQ(state.value.confirmed_location().x(), 50);
    const auto entered = a->scene()->enter_scene(3, soon());
    ASSERT_TRUE(entered.ok()) << entered.error_code;
    EXPECT_EQ(entered.value.location().shard_id(), 1);
    EXPECT_EQ(entered.value.location().x(), 50);
    ASSERT_TRUE(a->scene()->move_in_scene(4, 60, 0, soon()).ok());
    ASSERT_TRUE(b->scene()->move_in_scene(4, 220, 300, soon()).ok());
    const auto first = a->scene()->get_scene_state(soon());
    const auto second = b->scene()->get_scene_state(soon());
    ASSERT_TRUE(first.ok() && second.ok());
    ASSERT_EQ(first.value.snapshot().members_size(), 1);
    ASSERT_EQ(second.value.snapshot().members_size(), 1);
    EXPECT_EQ(first.value.snapshot().shard_id(), 1);
    EXPECT_EQ(second.value.snapshot().shard_id(), 2);
    EXPECT_NE(
        first.value.snapshot().shard_incarnation(),
        second.value.snapshot().shard_incarnation());
    const auto durable = store.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(durable && durable->owner);
    EXPECT_EQ(durable->owner->scene_instance_id, "scene-dev-02");
    EXPECT_EQ(durable->owner->shard_version, 2);
    EXPECT_EQ(
        durable->confirmed_location,
        std::optional(common::SceneLocation{1, 60, 0}));
    stale_write_rejected(*old, 3, {1, 60, 0});
    EXPECT_NE(
        metrics(realm_metrics_port_)
            .find("scene_shard_assignments_total{operation=\"recover\"} 1"),
        std::string::npos);
    EXPECT_NE(
        metrics(scene_metrics_ports_.at(root_b_.path()))
            .find("scene_owned_shards 2"),
        std::string::npos);
    RecordProperty(
        "database_verification",
        "rebuild: shard1 remains id1 at(50,0), owner B/version2 after A SIGKILL+lease expiry; Enter seq3 then Move seq4 at(60,0); B shard2 continues independently; old role write rejected");
}
TEST_F(
    SceneDynamicJourneyTest,
    ControlRequiresExactServerAndAuthorizedClientIdentity) {
    wait_shard(1, "scene-dev-01");
    const auto tls =
        std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY);
    auto wrong_server = control(
        false,
        false,
        "auth",
        1000,
        {"--server-identity", "scene-dev-02.scene.service"});
    EXPECT_EQ(wrong_server->wait(5s), 1);
    auto wrong_client = control(
        false,
        false,
        "auth",
        1000,
        {"--certificate",
         (tls / "scene2.pem").string(),
         "--key",
         (tls / "scene2.key").string()});
    EXPECT_EQ(wrong_client->wait(5s), 1);
    auto valid = control(false, false);
    ASSERT_EQ(valid->wait(5s), 0) << valid->output();
    EXPECT_EQ(valid->last().value("phase", "missing"), "idle");
    EXPECT_FALSE(database().scene_instance_state("scene-dev-01")->draining);
}
}  // namespace
}  // namespace realm::game::realm
