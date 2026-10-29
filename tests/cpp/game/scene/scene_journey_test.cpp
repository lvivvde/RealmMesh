#include "constrained_tls_stream.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/realm_protocol.hpp"
#include <fcntl.h>
#include <httplib.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <future>
#include <initializer_list>
#include <map>
#include <vector>
#include "../realm/journey_fixture.hpp"
#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/game/client/chat_session.hpp"
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
    virtual bool constrain_realm_output() const { return false; }
    virtual void stop_extra_scene() {}
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
        test_support::point_player_data_at(root, player_data(), database);
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
            "scene = {\n            enabled = false",
            "scene = {\n            enabled = true");
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
            player_data().uri(), database);
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
        if (constrain_realm_output())
            rewrite(realm_root_.path() / "services/realm.lua",
                    "max_pending_output_bytes = 1048576", "max_pending_output_bytes = 4096");
        scene_.emplace(scene_root_.path());
        wait_for_port(scene_port_, "scene");
    }
    void TearDown() override {
        RealmJourneyTest::TearDown();
        scene_.reset();
        stop_extra_scene();
        const auto endpoint = etcd_.endpoint();
        const auto etcd_port = static_cast<std::uint16_t>(
            std::stoul(endpoint.substr(endpoint.rfind(':') + 1)));
        etcd_.stop();
        verify_closed_ports({etcd_port});
        verify_closed_ports(
            {ports_.login_verify,
             ports_.queue,
             ports_.gateway,
             ports_.realm,
             scene_port_,
             realm_metrics_port_});
        for (const auto& [root, port] : scene_metrics_ports_)
            if (root == scene_root_.path()) verify_closed_ports({port});
        RecordProperty(
            "cleanup_verification",
            "entry, Realm, Scene A and etcd reaped; listener ports closed");
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
    /// 期望成功的 Enter/Move 容忍瞬时 3010,同一 scene_seq 重放(见
    /// retry_unavailable);期望特定错误码的调用不经此处。
    static client::RealmReply<common::SceneCommandResult> scene_enter(
        client::RealmSession& session, std::uint64_t seq) {
        return retry_unavailable([&](client::TimePoint deadline) {
            return session.scene()->enter_scene(seq, deadline);
        });
    }
    static client::RealmReply<common::SceneCommandResult> scene_move(
        client::RealmSession& session,
        std::uint64_t seq,
        std::int32_t x,
        std::int32_t y) {
        return retry_unavailable([&](client::TimePoint deadline) {
            return session.scene()->move_in_scene(seq, x, y, deadline);
        });
    }
    void verify_closed_ports(std::initializer_list<std::uint16_t> ports) {
        for (const auto port : ports) {
            if (port == 0) continue;
            const auto deadline = client::Clock::now() + 1s;
            while (test_support::loopback_port_open(port) &&
                   client::Clock::now() < deadline)
                std::this_thread::sleep_for(5ms);
            EXPECT_FALSE(test_support::loopback_port_open(port))
                << "listener remains on " << port;
        }
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
    const auto joined_a = scene_enter(*a, 1);
    ASSERT_TRUE(joined_a.ok()) << joined_a.error_code;
    ASSERT_TRUE(scene_enter(*b, 1).ok());
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
        const auto moved_a = scene_move(*a, i + 1, static_cast<int>(i * 10), 0);
        ASSERT_TRUE(moved_a.ok()) << moved_a.error_code;
        const auto seen_b =
            event(*b->scene(), common::SceneEvent::KIND_MOVE, ca);
        const auto seen_a =
            event(*a->scene(), common::SceneEvent::KIND_MOVE, ca);
        EXPECT_EQ(seen_a.SerializeAsString(), seen_b.SerializeAsString());
        EXPECT_EQ(seen_a.event_seq(), ++event_seq);
        EXPECT_EQ(seen_a.member().location().x(), static_cast<int>(i * 10));
        const auto moved_b = scene_move(*b, i + 1, 0, static_cast<int>(i * 10));
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
    common::MongoPlayerDataStore database(player_data().uri(), database_);
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
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_move(*a, 2, 10, 0).ok());
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
    RecordProperty(
        "database_verification",
        "Scene absent and Realm restart retain committed seq2 at(10,0); heartbeat/training remain available");
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
    void stop_extra_scene() override {
        scene_b_.reset();
        verify_closed_ports({port_b_});
        if (const auto it = scene_metrics_ports_.find(root_b_.path());
            it != scene_metrics_ports_.end())
            verify_closed_ports({it->second});
    }
    void TearDown() override {
        SceneJourneyTest::TearDown();
        RecordProperty(
            "cleanup_verification",
            "entry, Realm, Scene A/B and etcd reaped; listener ports closed");
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
        const std::string name =
            account == journey_account ? "TransferAlpha" : "TransferBeta";
        bool saw_unavailable = false;
        const auto hero = retry_unavailable(
            [&](client::TimePoint deadline) {
                return session->create_character(name, deadline);
            },
            &saw_unavailable);
        if (hero.ok()) {
            character = hero.value.character_id;
        } else if (
            saw_unavailable && hero.status == RealmCallStatus::Rejected &&
            hero.error_code == common::edge_error_character_name_taken) {
            // 先见 3010、重放才重名：只可能是那次被吞掉回包、实际已落库的
            // 建角。首次即重名仍按失败报告，不掩盖夹具污染或重复建角。
            const auto roster =
                retry_unavailable([&](client::TimePoint deadline) {
                    return session->list_characters(deadline);
                });
            const auto found = std::find_if(
                roster.value.characters.begin(),
                roster.value.characters.end(),
                [&](const auto& view) { return view.name == name; });
            if (!roster.ok() || found == roster.value.characters.end()) {
                ADD_FAILURE() << hero.error_code;
                return {};
            }
            character = found->character_id;
        } else {
            ADD_FAILURE() << hero.error_code;
            return {};
        }
        const auto chosen = retry_unavailable([&](client::TimePoint deadline) {
            return session->select_character(character, deadline);
        });
        if (!chosen.ok()) {
            ADD_FAILURE() << chosen.error_code;
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
        common::MongoPlayerDataStore database(player_data().uri(), database_);
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
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());
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
    ASSERT_TRUE(scene_move(*a, 3, 210, 300).ok());
    EXPECT_EQ(
        event(*b->scene(), common::SceneEvent::KIND_MOVE, ca)
            .member()
            .location()
            .x(),
        210);
    common::MongoPlayerDataStore database(player_data().uri(), database_);
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
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    common::MongoPlayerDataStore database(player_data().uri(), database_);
    const auto old = database.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(old);
    ASSERT_TRUE(old->owner);
    scene_b_->crash();
    ASSERT_TRUE(scene_move(*a, 2, 10, 0).ok());
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
    ASSERT_TRUE(scene_enter(*b, 3).ok());
    const auto recovered = database.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(recovered);
    ASSERT_TRUE(recovered->owner);
    EXPECT_NE(
        recovered->owner->scene_incarnation, old->owner->scene_incarnation);
    EXPECT_GT(recovered->binding_epoch, old->binding_epoch);
    EXPECT_EQ(recovered->confirmed_location, old->confirmed_location);
    ASSERT_TRUE(scene_move(*b, 4, 210, 300).ok());
    stale_write_rejected(*old, 5, {2, 210, 300});
    RecordProperty(
        "database_verification",
        "Scene SIGKILL and same-name restart retain target location; new incarnation/epoch; old binding rejected; other shard, heartbeat and training continue");
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
    ASSERT_TRUE(scene_enter(*first, 1).ok());
    common::MongoPlayerDataStore database(player_data().uri(), database_);
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
    ASSERT_TRUE(scene_enter(*again, 3).ok());
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
    RecordProperty(
        "database_verification",
        "Realm SIGKILL after target commit: seq2 replay cannot roll back target; new Realm epoch restores target location and rejects previous owner");
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
        return {player_data().uri(), database_, options};
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
        const auto path = scene_root_.path() / "logs" /
                          ("metrics-" + std::to_string(port) + ".prom");
        std::filesystem::create_directories(path.parent_path());
        std::ofstream sample(path);
        sample << response->body;
        sample.flush();
        EXPECT_TRUE(sample.good()) << "cannot persist metrics sample " << path;
        RecordProperty(
            "metrics_verification",
            "HTTP /metrics samples captured with service logs");
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

namespace chat_wire = ::realmmesh::protocol::chat::v1;

/// C4 使用生产入口组、真实 etcd/MongoDB、两个 Scene 与两个 Chat。
/// Realm Channel 和 shard 1 由 Chat A 承载，shard 2 由 Chat B 承载；
/// 发现只解析配置指定的精确 instance_id，故障时不改投另一实例。
class DualSceneDualChatJourneyTest : public SceneDynamicJourneyTest {
protected:
    void prepare_services(const std::string& database) override {
        SceneDynamicJourneyTest::prepare_services(database);
        const auto tls =
            std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY);
        set_environment(
            "REALMMESH_CHAT_CERTIFICATE_FILE", (tls / "chat.pem").string());
        set_environment(
            "REALMMESH_CHAT_PRIVATE_KEY_FILE", (tls / "chat.key").string());
        set_environment(
            "REALMMESH_CHAT2_CERTIFICATE_FILE", (tls / "chat2.pem").string());
        set_environment(
            "REALMMESH_CHAT2_PRIVATE_KEY_FILE", (tls / "chat2.key").string());

        const auto ports = test_support::unused_loopback_ports(4);
        chat_a_port_ = ports[0];
        chat_b_port_ = ports[1];
        chat_a_metrics_ = ports[2];
        chat_b_metrics_ = ports[3];
        configure_chat(
            chat_a_root_.path(),
            "chat-dev-01",
            "REALMMESH_CHAT_CERTIFICATE_FILE",
            "REALMMESH_CHAT_PRIVATE_KEY_FILE",
            chat_a_port_,
            chat_a_metrics_,
            "{ 0, 1 }");
        configure_chat(
            chat_b_root_.path(),
            "chat-dev-02",
            "REALMMESH_CHAT2_CERTIFICATE_FILE",
            "REALMMESH_CHAT2_PRIVATE_KEY_FILE",
            chat_b_port_,
            chat_b_metrics_,
            "{ 2 }");

        const auto realm_config = realm_root_.path() / "services/realm.lua";
        rewrite(
            realm_config,
            "chat = {\n            enabled = false",
            "chat = {\n            enabled = true");
        rewrite(
            realm_config,
            "targets = { { instance_id = \"chat-dev-01\",\n"
            "                server_identity = \"chat-dev-01.chat.service\",\n"
            "                host = \"127.0.0.1\", port = 7300, channels = {0, 1, 2} } },",
            "targets = {\n"
            "                { instance_id = \"chat-dev-01\", server_identity = \"chat-dev-01.chat.service\",\n"
            "                  host = \"127.0.0.1\", port = 1, channels = {0, 1} },\n"
            "                { instance_id = \"chat-dev-02\", server_identity = \"chat-dev-02.chat.service\",\n"
            "                  host = \"127.0.0.1\", port = 1, channels = {2} },\n"
            "            },");

        chat_a_.emplace(chat_a_root_.path());
        chat_b_.emplace(chat_b_root_.path());
        wait_for_port(chat_a_port_, "chat-a");
        wait_for_port(chat_b_port_, "chat-b");
    }

    void TearDown() override {
        chat_a_.reset();
        chat_b_.reset();
        verify_closed_ports(
            {chat_a_port_, chat_b_port_, chat_a_metrics_, chat_b_metrics_});
        SceneDynamicJourneyTest::TearDown();
        for (const auto* name :
             {"REALMMESH_CHAT_CERTIFICATE_FILE",
              "REALMMESH_CHAT_PRIVATE_KEY_FILE",
              "REALMMESH_CHAT2_CERTIFICATE_FILE",
              "REALMMESH_CHAT2_PRIVATE_KEY_FILE"})
            ::unsetenv(name);
        RecordProperty(
            "cleanup_verification",
            "entry, Realm, Scene A/B, Chat A/B, MongoDB fixture and etcd reaped; listener ports closed");
    }

    void configure_chat(
        const std::filesystem::path& root,
        std::string_view instance,
        std::string_view certificate_environment,
        std::string_view private_key_environment,
        std::uint16_t port,
        std::uint16_t metrics_port,
        std::string_view channels) {
        copy_configs(root, "return { services = { { name = \"chat\" } } }");
        const auto config = root / "services/chat.lua";
        if (instance != "chat-dev-01")
            rewrite(config, "chat-dev-01", std::string(instance));
        rewrite(
            config,
            "REALMMESH_CHAT_CERTIFICATE_FILE",
            std::string(certificate_environment));
        rewrite(
            config,
            "REALMMESH_CHAT_PRIVATE_KEY_FILE",
            std::string(private_key_environment));
        rewrite(
            config,
            "channels = { 0, 1, 2 }",
            "channels = " + std::string(channels));
        rewrite(
            config,
            "listen_port = 7300",
            "listen_port = " + std::to_string(port));
        rewrite(
            config,
            "metrics_port = 9107",
            "metrics_port = " + std::to_string(metrics_port));
        rewrite(
            root / "common/discovery.lua", "enabled = false", "enabled = true");
        rewrite(
            root / "common/discovery.lua",
            "http://127.0.0.1:2379",
            etcd_.endpoint());
    }

    common::ChatState active_chat(
        client::RealmSession& session,
        chat_wire::ChannelKind kind,
        std::optional<std::uint64_t> shard = {}) {
        const auto deadline = client::Clock::now() + 8s;
        while (client::Clock::now() < deadline) {
            const auto reply = session.chat()->get_chat_state(kind, soon());
            if (reply.ok() && reply.value.phase() == chat_wire::PHASE_ACTIVE &&
                (!shard || reply.value.epoch().channel().shard_id() == *shard))
                return reply.value;
            std::this_thread::sleep_for(10ms);
        }
        ADD_FAILURE() << "Chat Channel did not become active";
        return {};
    }

    common::ChatState unavailable_chat(
        client::RealmSession& session, chat_wire::ChannelKind kind) {
        const auto deadline = client::Clock::now() + 8s;
        while (client::Clock::now() < deadline) {
            const auto reply = session.chat()->get_chat_state(kind, soon());
            if (reply.ok() &&
                reply.value.phase() == chat_wire::PHASE_UNAVAILABLE)
                return reply.value;
            std::this_thread::sleep_for(10ms);
        }
        ADD_FAILURE() << "Chat failure did not become visible";
        return {};
    }

    static common::SendChat chat_send(
        const common::ChatState& state,
        std::uint64_t sequence,
        std::string text) {
        common::SendChat request;
        request.set_kind(state.kind());
        request.set_membership_generation(state.membership_generation());
        *request.mutable_expected_epoch() = state.epoch();
        request.set_client_send_seq(sequence);
        request.set_text(std::move(text));
        return request;
    }

    static client::RealmReply<common::ChatSendResult> send_chat(
        client::RealmSession& session,
        const common::ChatState& state,
        std::uint64_t sequence,
        std::string text) {
        return session.chat()->send_chat(
            chat_send(state, sequence, std::move(text)), soon());
    }

    std::vector<common::ChatMessage> chat_messages(
        client::RealmSession& session,
        chat_wire::ChannelKind kind,
        std::uint64_t shard,
        std::size_t count,
        std::chrono::milliseconds budget = 3s) {
        std::vector<common::ChatMessage> values;
        const auto deadline = client::Clock::now() + budget;
        while (values.size() < count && client::Clock::now() < deadline) {
            const auto notice = session.chat()->poll_chat(deadline);
            if (!notice) break;
            const auto* message = std::get_if<common::ChatMessage>(&*notice);
            if (message && message->epoch().channel().kind() == kind &&
                message->epoch().channel().shard_id() == shard)
                values.push_back(*message);
        }
        return values;
    }

    bool has_chat_message(
        client::RealmSession& session,
        chat_wire::ChannelKind kind,
        std::uint64_t shard,
        std::chrono::milliseconds budget = 300ms) {
        return !chat_messages(session, kind, shard, 1, budget).empty();
    }

    void verify_chat_metrics(std::uint16_t port) {
        httplib::Client http("127.0.0.1", port);
        const auto response = http.Get("/metrics");
        ASSERT_TRUE(response);
        ASSERT_EQ(response->status, 200);
        EXPECT_NE(response->body.find("chat_send_total"), std::string::npos);
    }

    test_support::TemporaryDirectory chat_a_root_{"c4-chat-a-"};
    test_support::TemporaryDirectory chat_b_root_{"c4-chat-b-"};
    std::optional<MeshProcess> chat_a_, chat_b_;
    std::uint16_t chat_a_port_{0}, chat_b_port_{0};
    std::uint16_t chat_a_metrics_{0}, chat_b_metrics_{0};
};

TEST_F(
    DualSceneDualChatJourneyTest,
    StaticOwnersIsolateTransfersAndOneChatFailureWhileRealmChannelContinues) {
    wait_shard(1, "scene-dev-01");
    wait_shard(2, "scene-dev-02");
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());

    const auto realm_a = active_chat(*a, chat_wire::CHANNEL_KIND_REALM, 0);
    const auto realm_b = active_chat(*b, chat_wire::CHANNEL_KIND_REALM, 0);
    auto sent = send_chat(*a, realm_a, 1, "realm-a");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    sent = send_chat(*b, realm_b, 1, "realm-b");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    const auto realm_seen_a =
        chat_messages(*a, chat_wire::CHANNEL_KIND_REALM, 0, 2);
    const auto realm_seen_b =
        chat_messages(*b, chat_wire::CHANNEL_KIND_REALM, 0, 2);
    ASSERT_EQ(realm_seen_a.size(), 2);
    ASSERT_EQ(realm_seen_b.size(), 2);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(realm_seen_a[i].channel_seq(), i + 1);
        EXPECT_EQ(realm_seen_b[i].channel_seq(), i + 1);
        EXPECT_EQ(
            realm_seen_a[i].sender_character_id(),
            realm_seen_b[i].sender_character_id());
        EXPECT_EQ(realm_seen_a[i].text(), realm_seen_b[i].text());
    }

    const auto scene_a = active_chat(*a, chat_wire::CHANNEL_KIND_SCENE, 1);
    const auto old_scene_b = active_chat(*b, chat_wire::CHANNEL_KIND_SCENE, 1);
    sent = send_chat(*a, scene_a, 1, "shared-scene");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*a, chat_wire::CHANNEL_KIND_SCENE, 1, 1).size(), 1);
    ASSERT_EQ(chat_messages(*b, chat_wire::CHANNEL_KIND_SCENE, 1, 1).size(), 1);

    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    const auto scene_b = active_chat(*b, chat_wire::CHANNEL_KIND_SCENE, 2);
    EXPECT_NE(
        scene_b.membership_generation(), old_scene_b.membership_generation());
    const auto stale = send_chat(*b, old_scene_b, 1, "stale-source");
    ASSERT_TRUE(stale.ok());
    EXPECT_EQ(stale.value.outcome(), chat_wire::OUTCOME_REJECTED);
    EXPECT_EQ(stale.value.reason(), chat_wire::REASON_STALE_MEMBERSHIP);

    sent = send_chat(*a, scene_a, 2, "only-shard-one");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*a, chat_wire::CHANNEL_KIND_SCENE, 1, 1).size(), 1);
    EXPECT_FALSE(has_chat_message(*b, chat_wire::CHANNEL_KIND_SCENE, 1));
    sent = send_chat(*b, scene_b, 1, "only-shard-two");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*b, chat_wire::CHANNEL_KIND_SCENE, 2, 1).size(), 1);
    EXPECT_FALSE(has_chat_message(*a, chat_wire::CHANNEL_KIND_SCENE, 2));

    const auto after_transfer_realm =
        active_chat(*b, chat_wire::CHANNEL_KIND_REALM, 0);
    EXPECT_EQ(
        after_transfer_realm.membership_generation(),
        realm_b.membership_generation());
    EXPECT_EQ(
        after_transfer_realm.epoch().SerializeAsString(),
        realm_b.epoch().SerializeAsString());
    sent = send_chat(*a, realm_a, 2, "realm-after-transfer");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    const auto continued_a =
        chat_messages(*a, chat_wire::CHANNEL_KIND_REALM, 0, 1);
    const auto continued_b =
        chat_messages(*b, chat_wire::CHANNEL_KIND_REALM, 0, 1);
    ASSERT_EQ(continued_a.size(), 1);
    ASSERT_EQ(continued_b.size(), 1);
    EXPECT_EQ(continued_a.front().channel_seq(), 3);
    EXPECT_EQ(continued_b.front().channel_seq(), 3);

    common::MongoPlayerDataStore store(player_data().uri(), database_);
    const auto old = store.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(old && old->owner);
    const auto old_chat_b_incarnation = scene_b.epoch().chat_incarnation();
    chat_b_->crash();
    chat_b_.reset();
    verify_closed_ports({chat_b_port_, chat_b_metrics_});
    const auto failed = send_chat(*b, scene_b, 2, "chat-b-down");
    ASSERT_TRUE(failed.ok());
    EXPECT_NE(failed.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    EXPECT_EQ(
        unavailable_chat(*b, chat_wire::CHANNEL_KIND_SCENE).phase(),
        chat_wire::PHASE_UNAVAILABLE);

    ASSERT_TRUE(scene_move(*b, 3, 210, 300).ok());
    ASSERT_TRUE(b->train(1, soon()).ok());
    ASSERT_TRUE(b->heartbeat(soon()));
    ASSERT_TRUE(b->scene()->transfer_scene(4, 1, soon()).ok());
    const auto returned = active_chat(*b, chat_wire::CHANNEL_KIND_SCENE, 1);
    sent = send_chat(*b, returned, 1, "returned-to-shard-one");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*a, chat_wire::CHANNEL_KIND_SCENE, 1, 1).size(), 1);
    ASSERT_EQ(chat_messages(*b, chat_wire::CHANNEL_KIND_SCENE, 1, 1).size(), 1);

    wait_instance_expired("chat", "chat-dev-02");
    chat_b_.emplace(chat_b_root_.path());
    wait_for_port(chat_b_port_, "restarted chat-b");
    ASSERT_TRUE(b->scene()->transfer_scene(5, 2, soon()).ok());
    const auto restarted = active_chat(*b, chat_wire::CHANNEL_KIND_SCENE, 2);
    EXPECT_NE(restarted.epoch().chat_incarnation(), old_chat_b_incarnation);
    EXPECT_GT(
        restarted.membership_generation(), scene_b.membership_generation());
    EXPECT_EQ(restarted.join_watermark(), 0);
    sent = send_chat(*b, restarted, 1, "new-chat-epoch");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    const auto restarted_message =
        chat_messages(*b, chat_wire::CHANNEL_KIND_SCENE, 2, 1);
    ASSERT_EQ(restarted_message.size(), 1);
    EXPECT_EQ(restarted_message.front().channel_seq(), 1);

    const auto final_realm = active_chat(*b, chat_wire::CHANNEL_KIND_REALM, 0);
    EXPECT_EQ(
        final_realm.membership_generation(), realm_b.membership_generation());
    EXPECT_EQ(
        final_realm.epoch().SerializeAsString(),
        realm_b.epoch().SerializeAsString());
    sent = send_chat(*b, final_realm, 2, "realm-never-reset");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*a, chat_wire::CHANNEL_KIND_REALM, 0, 1).size(), 1);
    ASSERT_EQ(chat_messages(*b, chat_wire::CHANNEL_KIND_REALM, 0, 1).size(), 1);

    const auto durable = store.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(durable && durable->owner && durable->last_result);
    EXPECT_EQ(durable->last_result->command.seq, 5);
    EXPECT_EQ(durable->owner->scene_instance_id, "scene-dev-02");
    EXPECT_EQ(
        durable->confirmed_location,
        std::optional(common::SceneLocation{2, 200, 300}));
    stale_write_rejected(*old, 6, {2, 200, 300});
    verify_chat_metrics(chat_a_metrics_);
    verify_chat_metrics(chat_b_metrics_);
    RecordProperty(
        "database_verification",
        "transfer seq2/4/5 and move seq3 persisted; old binding write fenced; Chat B failure did not reset Chat A Realm Channel");
    RecordProperty(
        "chat_topology_verification",
        "Chat A owns Realm+shard1, Chat B owns shard2; no failover; same-name restart created a new Epoch/W");
}

TEST_F(
    DualSceneDualChatJourneyTest,
    SameShardRebuildKeepsLogicalChannelAndInstallsNewBindingQualification) {
    wait_shard(1, "scene-dev-01");
    wait_shard(2, "scene-dev-02");
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_move(*a, 2, 50, 0).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    const auto realm_before = active_chat(*a, chat_wire::CHANNEL_KIND_REALM, 0);
    const auto scene_before = active_chat(*a, chat_wire::CHANNEL_KIND_SCENE, 1);
    auto sent = send_chat(*a, scene_before, 1, "before-rebuild");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*a, chat_wire::CHANNEL_KIND_SCENE, 1, 1).size(), 1);

    auto store = database();
    const auto old = store.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(old && old->owner);
    scene_->crash();
    scene_.reset();
    ASSERT_TRUE(scene_move(*b, 3, 210, 300).ok());
    const auto unaffected_b = active_chat(*b, chat_wire::CHANNEL_KIND_SCENE, 2);
    sent = send_chat(*b, unaffected_b, 1, "other-shard-continues");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*b, chat_wire::CHANNEL_KIND_SCENE, 2, 1).size(), 1);
    wait_instance_expired("scene", "scene-dev-01");
    const auto recovered_owner = wait_shard(1, "scene-dev-02");
    EXPECT_EQ(recovered_owner.version, 2);
    ASSERT_TRUE(scene_enter(*a, 3).ok());
    const auto scene_after = active_chat(*a, chat_wire::CHANNEL_KIND_SCENE, 1);
    EXPECT_EQ(
        scene_after.epoch().channel().SerializeAsString(),
        scene_before.epoch().channel().SerializeAsString());
    EXPECT_EQ(
        scene_after.epoch().chat_incarnation(),
        scene_before.epoch().chat_incarnation());
    EXPECT_GT(
        scene_after.membership_generation(),
        scene_before.membership_generation());
    EXPECT_GE(scene_after.join_watermark(), 1);
    const auto stale = send_chat(*a, scene_before, 2, "old-binding");
    ASSERT_TRUE(stale.ok());
    EXPECT_EQ(stale.value.outcome(), chat_wire::OUTCOME_REJECTED);
    sent = send_chat(*a, scene_after, 1, "after-rebuild");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    const auto after_message =
        chat_messages(*a, chat_wire::CHANNEL_KIND_SCENE, 1, 1);
    ASSERT_EQ(after_message.size(), 1);
    EXPECT_GT(after_message.front().channel_seq(), 1);
    ASSERT_TRUE(scene_move(*a, 4, 60, 0).ok());

    const auto realm_after = active_chat(*a, chat_wire::CHANNEL_KIND_REALM, 0);
    EXPECT_EQ(
        realm_after.membership_generation(),
        realm_before.membership_generation());
    EXPECT_EQ(
        realm_after.epoch().SerializeAsString(),
        realm_before.epoch().SerializeAsString());
    const auto durable = store.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(durable && durable->owner && durable->last_result);
    EXPECT_EQ(durable->last_result->command.seq, 4);
    EXPECT_EQ(durable->owner->scene_instance_id, "scene-dev-02");
    EXPECT_EQ(durable->owner->shard_version, 2);
    EXPECT_EQ(
        durable->confirmed_location,
        std::optional(common::SceneLocation{1, 60, 0}));
    stale_write_rejected(*old, 5, {1, 60, 0});
    RecordProperty(
        "database_verification",
        "shard1 stayed logical id1 after carrier rebuild; binding/owner version advanced; seq4 at(60,0); old write fenced");
    RecordProperty(
        "chat_topology_verification",
        "same Scene Channel/Epoch survived carrier rebuild, new Scene Binding installed a higher membership generation");
}

TEST_F(
    DualSceneDualChatJourneyTest,
    SceneDrainMovesMembershipToTargetChatWithoutResettingRealmChannel) {
    wait_shard(1, "scene-dev-01");
    wait_shard(2, "scene-dev-02");
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_move(*a, 2, 10, 0).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    const auto realm_before = active_chat(*a, chat_wire::CHANNEL_KIND_REALM, 0);
    const auto realm_b_before =
        active_chat(*b, chat_wire::CHANNEL_KIND_REALM, 0);
    const auto source = active_chat(*a, chat_wire::CHANNEL_KIND_SCENE, 1);
    static_cast<void>(active_chat(*b, chat_wire::CHANNEL_KIND_SCENE, 2));
    auto store = database();
    const auto old = store.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(old && old->owner);

    auto draining = control(false, true, "chat-c4-drain");
    ASSERT_TRUE(scene_move(*b, 3, 210, 300).ok());
    ASSERT_TRUE(b->heartbeat(soon()));
    ASSERT_TRUE(b->train(1, soon()).ok());
    ASSERT_EQ(draining->wait(), 0) << draining->output();
    EXPECT_EQ(draining->last().value("phase", "missing"), "complete");
    const auto target = active_chat(*a, chat_wire::CHANNEL_KIND_SCENE, 2);
    EXPECT_NE(target.membership_generation(), source.membership_generation());
    EXPECT_EQ(target.epoch().channel().shard_id(), 2);
    const auto stale = send_chat(*a, source, 1, "drained-source");
    ASSERT_TRUE(stale.ok());
    EXPECT_EQ(stale.value.outcome(), chat_wire::OUTCOME_REJECTED);
    auto sent = send_chat(*a, target, 1, "drained-target");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*a, chat_wire::CHANNEL_KIND_SCENE, 2, 1).size(), 1);
    ASSERT_EQ(chat_messages(*b, chat_wire::CHANNEL_KIND_SCENE, 2, 1).size(), 1);

    const auto realm_after = active_chat(*a, chat_wire::CHANNEL_KIND_REALM, 0);
    const auto realm_b_after =
        active_chat(*b, chat_wire::CHANNEL_KIND_REALM, 0);
    EXPECT_EQ(
        realm_after.membership_generation(),
        realm_before.membership_generation());
    EXPECT_EQ(
        realm_after.epoch().SerializeAsString(),
        realm_before.epoch().SerializeAsString());
    EXPECT_EQ(
        realm_b_after.membership_generation(),
        realm_b_before.membership_generation());
    EXPECT_EQ(
        realm_b_after.epoch().SerializeAsString(),
        realm_b_before.epoch().SerializeAsString());
    sent = send_chat(*a, realm_after, 1, "realm-during-drain");
    ASSERT_TRUE(sent.ok());
    ASSERT_EQ(sent.value.outcome(), chat_wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(chat_messages(*a, chat_wire::CHANNEL_KIND_REALM, 0, 1).size(), 1);
    ASSERT_EQ(chat_messages(*b, chat_wire::CHANNEL_KIND_REALM, 0, 1).size(), 1);

    const auto durable = store.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(durable && durable->owner && durable->last_result);
    EXPECT_EQ(durable->last_result->command.seq, 3);
    EXPECT_EQ(durable->owner->scene_instance_id, "scene-dev-02");
    EXPECT_EQ(
        durable->confirmed_location,
        std::optional(common::SceneLocation{2, 200, 300}));
    const auto released = store.shard_state(1, 1);
    ASSERT_TRUE(released);
    EXPECT_FALSE(released->owner);
    stale_write_rejected(*old, 4, {2, 200, 300});
    RecordProperty(
        "database_verification",
        "drain committed transfer seq3 to shard2, released shard1 only after migration, and fenced the old binding write");
    RecordProperty(
        "chat_topology_verification",
        "drain retired shard1 qualification, installed shard2 qualification on Chat B, and kept Realm Channel on Chat A");
}

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
    ASSERT_TRUE(scene_enter(*player, 1).ok());
    ASSERT_TRUE(scene_move(*player, 2, 30, 0).ok());
    const auto before = store.scene_state({journey_account_id, 1, character});
    ASSERT_TRUE(before && before->owner);
    start_b();
    const auto second = wait_shard(2, "scene-dev-02");
    EXPECT_EQ(store.shard_state(1, 1), std::optional(first));
    EXPECT_EQ(second.version, 1);
    ASSERT_TRUE(scene_move(*player, 3, 40, 0).ok());
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
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_move(*a, 2, 10, 0).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    auto store = database();
    const auto old = store.scene_state({journey_account_id, 1, ca});
    auto draining = control(false, true, "drain-success");
    ASSERT_TRUE(scene_move(*b, 3, 210, 300).ok());
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
    ASSERT_TRUE(scene_move(*a, 4, 210, 300).ok());
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
    ASSERT_TRUE(scene_enter(*a, 1).ok());
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
            ASSERT_TRUE(scene_enter(*session, 1).ok());
            ASSERT_TRUE(session->scene()->transfer_scene(2, 2, soon()).ok());
            occupants.push_back(std::move(session));
        }
    }
    auto draining = control(false, true, "full-target", 1200);
    ASSERT_TRUE(scene_move(*a, 2, 10, 0).ok());
    ASSERT_TRUE(scene_move(*occupants.front(), 3, 210, 300).ok());
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
    ASSERT_TRUE(scene_move(*a, 3, 20, 0).ok());
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
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_move(*a, 2, 50, 0).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    auto store = database();
    const auto old = store.scene_state({journey_account_id, 1, ca});
    scene_->crash();
    scene_.reset();
    ASSERT_TRUE(scene_move(*b, 3, 210, 300).ok());
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
    const auto entered = scene_enter(*a, 3);
    ASSERT_TRUE(entered.ok()) << entered.error_code;
    EXPECT_EQ(entered.value.location().shard_id(), 1);
    EXPECT_EQ(entered.value.location().x(), 50);
    ASSERT_TRUE(scene_move(*a, 4, 60, 0).ok());
    ASSERT_TRUE(scene_move(*b, 4, 220, 300).ok());
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
// 故障只作用于此夹具独占副本集，永不影响 CTest 共享后端。
class SceneFaultJourneyTest : public SceneDynamicJourneyTest {
protected:
    const test_support::MongodProcess& player_data() const override {
        return fault_mongo_;
    }
    common::SceneShardRecord wait_restarted_shard(
        std::uint64_t shard, const common::SceneOwner& old) {
        auto store = database();
        const auto deadline = client::Clock::now() + 10s;
        while (client::Clock::now() < deadline) {
            const auto value = store.shard_state(1, shard);
            if (value && value->owner &&
                value->owner->instance_id == old.scene_instance_id &&
                value->owner->incarnation != old.scene_incarnation &&
                value->version > old.shard_version)
                return *value;
            std::this_thread::sleep_for(10ms);
        }
        throw std::runtime_error(
            "new carrier incarnation/version did not become ready");
    }

    void TearDown() override {
        // 断言提前失败时也先解除暂停，再停止依赖数据库的服务。
        try {
            fault_mongo_.resume();
        } catch (const std::exception& error) {
            ADD_FAILURE() << error.what();
        }
        SceneDynamicJourneyTest::TearDown();
        const auto port = fault_mongo_.port();
        fault_mongo_.stop();
        verify_closed_ports({port});
        RecordProperty(
            "backend_cleanup_verification",
            "private MongoDB reaped; listener port closed");
    }
    test_support::MongodProcess fault_mongo_;
};
TEST_F(
    SceneFaultJourneyTest,
    MongoWriteOutageThenCarrierLossPreservesConfirmedLocation) {
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_move(*a, 2, 50, 0).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    auto store = database();
    const auto old = store.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(old && old->owner);
    fault_mongo_.pause();
    const auto failed = a->scene()->move_in_scene(3, 60, 0, soon());
    EXPECT_EQ(failed.error_code, 3104);
    EXPECT_TRUE(a->heartbeat(soon()));
    EXPECT_TRUE(b->heartbeat(soon()));
    fault_mongo_.resume();
    EXPECT_EQ(store.scene_state(old->character), old);
    // 后端暂停也使 Scene 健康链路失效；查询已确认落点后显式重新进入。
    const auto available = a->scene()->get_scene_state(soon());
    ASSERT_TRUE(available.ok());
    EXPECT_EQ(available.value.last_scene_seq(), 2);
    EXPECT_EQ(available.value.confirmed_location().x(), 50);
    const auto reentered = scene_enter(*a, 3);
    ASSERT_TRUE(reentered.ok()) << reentered.error_code;
    ASSERT_TRUE(scene_move(*a, 4, 60, 0).ok());
    ASSERT_TRUE(scene_enter(*b, 3).ok());
    ASSERT_TRUE(scene_move(*b, 4, 210, 300).ok());
    ASSERT_TRUE(a->train(1, soon()).ok());
    ASSERT_TRUE(b->train(1, soon()).ok());
    scene_->crash();
    const auto rebuilt = wait_shard(1, "scene-dev-02");
    EXPECT_EQ(rebuilt.version, 2);
    const auto state = a->scene()->get_scene_state(soon());
    ASSERT_TRUE(state.ok());
    EXPECT_EQ(state.value.last_scene_seq(), 4);
    EXPECT_EQ(state.value.confirmed_location().x(), 60);
    ASSERT_TRUE(scene_enter(*a, 5).ok());
    ASSERT_TRUE(scene_move(*a, 6, 70, 0).ok());
    ASSERT_TRUE(scene_move(*b, 5, 220, 300).ok());
    stale_write_rejected(*old, 5, {1, 70, 0});
    const auto durable = store.scene_state(old->character);
    ASSERT_TRUE(durable && durable->owner);
    EXPECT_EQ(durable->last_result->command.seq, 6);
    EXPECT_EQ(durable->owner->shard_version, 2);
    static_cast<void>(metrics(realm_metrics_port_));
    RecordProperty(
        "database_verification",
        "private MongoDB SIGSTOP/SIGCONT: seq2 at(50,0) unchanged; read-stage 3104; Get then Enter seq3 and Move seq4 commit(60,0); A SIGKILL rebuilds shard1 on B/version2; seq6 at(70,0); other shard moves and heartbeat/training recover; old owner fenced");
    RecordProperty(
        "fault_matrix_verification",
        "mongodb_process_pause+reenter+move_retry+Scene_SIGKILL+same_shard_rebuild");
}
TEST_F(
    SceneFaultJourneyTest,
    TargetCrashDrainFailureAndSameNameRestartRetryRemainFenced) {
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto b = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(a && b);
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_move(*a, 2, 30, 0).ok());
    ASSERT_TRUE(scene_enter(*b, 1).ok());
    ASSERT_TRUE(b->scene()->transfer_scene(2, 2, soon()).ok());
    auto store = database();
    const auto old_a = store.scene_state({journey_account_id, 1, ca});
    const auto old_b = store.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(old_a && old_a->owner && old_b && old_b->owner);
    scene_b_->crash();
    auto draining = control(false, true, "target-offline", 600);
    ASSERT_EQ(draining->wait(8s), 2) << draining->output();
    EXPECT_EQ(draining->last().value("phase", "missing"), "failed");
    ASSERT_TRUE(store.shard_state(1, 1)->draining);
    ASSERT_TRUE(scene_move(*a, 3, 40, 0).ok());
    ASSERT_TRUE(a->heartbeat(soon()));
    ASSERT_TRUE(b->heartbeat(soon()));
    ASSERT_TRUE(a->train(1, soon()).ok());
    wait_instance_expired("scene", "scene-dev-02");
    scene_b_.reset();
    start_b();
    const auto restored = wait_restarted_shard(2, *old_b->owner);
    EXPECT_EQ(restored.version, 2);
    EXPECT_NE(restored.owner->incarnation, old_b->owner->scene_incarnation);
    ASSERT_TRUE(scene_enter(*b, 3).ok());
    ASSERT_TRUE(scene_move(*b, 4, 220, 300).ok());
    auto retry = control(false, true, "target-restored", 20000);
    ASSERT_EQ(retry->wait(), 0) << retry->output();
    EXPECT_EQ(retry->last().value("phase", "missing"), "complete");
    const auto durable = store.scene_state(old_a->character);
    ASSERT_TRUE(durable && durable->owner);
    EXPECT_EQ(durable->last_result->command.seq, 4);
    EXPECT_EQ(
        durable->confirmed_location,
        std::optional(common::SceneLocation{2, 200, 300}));
    EXPECT_EQ(durable->owner->shard_version, 2);
    stale_write_rejected(*old_a, 5, {2, 200, 300});
    stale_write_rejected(*old_b, 5, {2, 220, 300});
    ASSERT_TRUE(b->heartbeat(soon()));
    ASSERT_TRUE(b->train(1, soon()).ok());
    static_cast<void>(metrics(realm_metrics_port_));
    static_cast<void>(metrics(scene_metrics_ports_.at(scene_root_.path())));
    RecordProperty(
        "database_verification",
        "B SIGKILL: A drain fails and keeps seq3 movement; B same-name restart owns shard2/version2; new drain attempt transfers A at seq4; both previous owners rejected; B seq4 remains(220,300)");
    RecordProperty(
        "fault_matrix_verification",
        "target_SIGKILL+drain_deadline+same_name_restart+new_attempt");
}
TEST_F(
    SceneFaultJourneyTest,
    SlowConsumerOverflowRejoinsFromDurableStateWithoutBlockingOtherShard) {
    std::uint64_t ca = 0, cb = 0;
    auto a = selected(ca);
    auto slow = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(a && slow);
    ASSERT_TRUE(scene_enter(*a, 1).ok());
    ASSERT_TRUE(scene_enter(*slow, 1).ok());
    auto store = database();
    store.provision_account(
        {.account_id = 7003,
         .account_name = "scene-healthy",
         .credential = "third-secret",
         .banned = false,
         .whitelisted = true});
    auto healthy = login("scene-healthy", "third-secret");
    ASSERT_TRUE(healthy);
    const auto hero = healthy->create_character("SlowHealthy", soon());
    ASSERT_TRUE(hero.ok());
    ASSERT_TRUE(
        healthy->select_character(hero.value.character_id, soon()).ok());
    ASSERT_TRUE(scene_enter(*healthy, 1).ok());
    ASSERT_TRUE(healthy->scene()->transfer_scene(2, 2, soon()).ok());
    const auto old = store.scene_state({second_account_id, 1, cb});
    ASSERT_TRUE(old && old->owner);
    // 257 是参考客户端 256 条通知边界的最小越界规模，两平台保持相同语义。
    auto next_heartbeat = client::Clock::now() + 1s;
    for (std::uint64_t i = 1; i <= 257; ++i) {
        const auto x = static_cast<std::int32_t>(i % 2 == 1 ? 10 : 20);
        const auto moved = scene_move(*a, i + 1, x, 0);
        ASSERT_TRUE(moved.ok()) << "move " << i << ": " << moved.error_code;
        while (a->scene()->poll_scene(client::Clock::now() + 1ms)) {
        }
        // CI 单次持久移动更慢；按时间保活，不能依赖累计次数赶在空闲截止前。
        if (client::Clock::now() >= next_heartbeat) {
            ASSERT_TRUE(healthy->heartbeat(soon()));
            next_heartbeat = client::Clock::now() + 1s;
        }
        if (i % 64 == 0) {
            const auto step = i / 64;
            ASSERT_TRUE(scene_move(
                            *healthy,
                            step + 2,
                            200 + static_cast<std::int32_t>(step) * 10,
                            300)
                            .ok());
            ASSERT_TRUE(healthy->heartbeat(soon()));
        }
    }
    EXPECT_FALSE(slow->heartbeat(soon()));
    slow->close();
    ASSERT_TRUE(a->heartbeat(soon()));
    ASSERT_TRUE(a->train(1, soon()).ok());
    ASSERT_TRUE(healthy->train(1, soon()).ok());
    const auto durable_a = store.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(durable_a);
    EXPECT_EQ(durable_a->last_result->command.seq, 258);
    EXPECT_EQ(
        durable_a->confirmed_location,
        std::optional(common::SceneLocation{1, 10, 0}));
    auto again = login("scene-second", "second-secret");
    ASSERT_TRUE(again);
    ASSERT_TRUE(again->select_character(cb, soon()).ok());
    const auto before = again->scene()->get_scene_state(soon());
    ASSERT_TRUE(before.ok());
    EXPECT_EQ(before.value.last_scene_seq(), 1);
    EXPECT_EQ(before.value.confirmed_location().x(), 0);
    ASSERT_TRUE(scene_enter(*again, 2).ok());
    stale_write_rejected(*old, 3, {1, 0, 0});
    const auto snapshot = again->scene()->get_scene_state(soon());
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(positions(snapshot.value).at(ca), *durable_a->confirmed_location);
    const auto other = healthy->scene()->get_scene_state(soon());
    ASSERT_TRUE(other.ok());
    EXPECT_EQ(other.value.snapshot().shard_id(), 2);
    EXPECT_EQ(other.value.last_scene_seq(), 6);
    EXPECT_EQ(other.value.confirmed_location().x(), 240);
    EXPECT_EQ(other.value.snapshot().members_size(), 1);
    static_cast<void>(metrics(realm_metrics_port_));
    RecordProperty(
        "database_verification",
        "257 real committed moves: A seq258 at(10,0); only slow notification consumer closes; re-login keeps seq1 at(0,0), Enter seq2 fences prior binding; other shard seq6 at(240,300), heartbeat/training and snapshot intact");
    RecordProperty(
        "fault_matrix_verification",
        "slow_consumer+notification_overflow+relogin+other_shard_isolation");
}
class SceneOutboundJourneyTest : public SceneFaultJourneyTest {
protected:
    bool constrain_realm_output() const override { return true; }
    bool output_rejected() const {
        for (const auto& file : std::filesystem::recursive_directory_iterator(
                 realm_root_.path() / "logs")) {
            if (!file.is_regular_file()) continue;
            std::ifstream input(file.path());
            std::string line;
            while (std::getline(input, line)) {
                if (line.find("outbound_queue_rejected") == std::string::npos)
                    continue;
                const auto event = nlohmann::json::parse(line, nullptr, false);
                if (event.is_discarded()) continue;
                // 小回复、已有积压的 4KiB 上限排除单帧超限和业务队列拒绝。
                const auto& attributes = event.at("attributes");
                return attributes.at("max_pending_output_bytes") == 4096 &&
                       attributes.at("has_pending_output") == true &&
                       attributes.at("payload_bytes").get<std::size_t>() < 4096;
            }
        }
        return false;
    }
};
TEST_F(
    SceneOutboundJourneyTest,
    ServerOutputSaturationClosesOnlySlowSessionAndRestoresBinding) {
    std::uint64_t ca = 0, cb = 0;
    auto initial = selected(ca);
    auto healthy = selected(cb, "scene-second", "second-secret");
    ASSERT_TRUE(initial && healthy);
    ASSERT_TRUE(scene_enter(*initial, 1).ok());
    ASSERT_TRUE(scene_move(*initial, 2, 50, 0).ok());
    ASSERT_TRUE(scene_enter(*healthy, 1).ok());
    ASSERT_TRUE(healthy->scene()->transfer_scene(2, 2, soon()).ok());
    initial->close();
    // 只捕获未消费的 EnterRealm 票据；在收紧接收窗口的真实 TLS 上兑换。
    class TicketCapture final : public client::EnterRealmRedeemer {
    public:
        std::string ticket;
        client::PortStatus redeem(
            network::client::ISecureByteStream&,
            std::string_view value,
            client::TimePoint) override {
            ticket.assign(value);
            return client::PortStatus::success();
        }
    } capture;
    client::WireEndpoints endpoints;
    endpoints.login_verify_port = ports_.login_verify;
    endpoints.queue_port = ports_.queue;
    endpoints.verify_peer = false;
    client::WireLoginTransport transport(endpoints, capture);
    auto unused = login(journey_account, journey_credential, &transport);
    ASSERT_TRUE(unused);
    unused->close();
    test_support::ConstrainedTlsStream slow(ports_.realm, soon());
    ASSERT_TRUE(redeemer_.redeem(slow, capture.ticket, soon()).ok);
    capture.ticket.clear();
    network::client::EdgeClientConnection edge(slow);
    common::SelectCharacter select;
    select.set_character_id(ca);
    ASSERT_TRUE(edge.send_frame(common::encode(select, 1), soon()));
    auto reply = edge.receive_frame(soon());
    ASSERT_TRUE(reply && common::decode_character_selected(*reply));
    common::EnterScene enter;
    enter.set_scene_seq(3);
    ASSERT_TRUE(edge.send_frame(common::encode(enter, 2), soon()));
    reply = edge.receive_frame(soon());
    ASSERT_TRUE(reply && common::decode_scene_command_result(*reply));
    auto store = database();
    const auto old = store.scene_state({journey_account_id, 1, ca});
    ASSERT_TRUE(old && old->owner && old->last_result->command.seq == 3);
    // 从此停止读取；仅生产心跳回包产生压力，不经过 Scene/数据等待队列。
    network::LengthFieldCodec codec(4096);
    std::vector<std::byte> batch;
    for (std::uint64_t i = 0; i < 512; ++i) {
        const auto frame = codec.encode(
            common::encode(common::HeartbeatRequest{}, 1000000000000ULL + i));
        batch.insert(batch.end(), frame.begin(), frame.end());
    }
    const auto deadline = client::Clock::now() + 60s;
    std::size_t batches = 0;
    while (!output_rejected() && client::Clock::now() < deadline &&
           batches++ < 600) {
        if (!slow.write_all(batch, client::Clock::now() + 1s)) break;
        if (batches % 16 == 0) ASSERT_TRUE(healthy->heartbeat(soon()));
        std::this_thread::sleep_for(10ms);
    }
    // 日志异步刷盘；写入失败与拒绝事件可见之间允许短暂传播。
    const auto log_deadline = client::Clock::now() + 2s;
    while (!output_rejected() && client::Clock::now() < log_deadline)
        std::this_thread::sleep_for(5ms);
    ASSERT_TRUE(output_rejected()) << "server output watermark was not reached";
    ASSERT_TRUE(healthy->heartbeat(soon()));
    ASSERT_TRUE(scene_move(*healthy, 3, 210, 300).ok());
    ASSERT_TRUE(healthy->train(1, soon()).ok());
    // 保持慢端仍打开，等待服务端关闭后撤销旧绑定；排除客户端主动 close。
    bool revoked = false;
    const auto revoke_deadline = client::Clock::now() + 5s;
    while (client::Clock::now() < revoke_deadline) {
        const auto value = store.scene_state(old->character);
        if (value && !value->owner) {
            revoked = true;
            break;
        }
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(revoked);
    slow.shutdown();
    auto again = login();
    ASSERT_TRUE(again && again->select_character(ca, soon()).ok());
    const auto restored = again->scene()->get_scene_state(soon());
    ASSERT_TRUE(restored.ok());
    EXPECT_EQ(restored.value.last_scene_seq(), 3);
    EXPECT_EQ(restored.value.confirmed_location().x(), 50);
    ASSERT_TRUE(scene_enter(*again, 4).ok());
    ASSERT_TRUE(scene_move(*again, 5, 60, 0).ok());
    stale_write_rejected(*old, 6, {1, 60, 0});
    const auto durable = store.scene_state(old->character);
    ASSERT_TRUE(durable && durable->owner);
    EXPECT_EQ(durable->last_result->command.seq, 5);
    EXPECT_EQ(
        durable->confirmed_location,
        std::optional(common::SceneLocation{1, 60, 0}));
    static_cast<void>(metrics(realm_metrics_port_));
    RecordProperty(
        "database_verification",
        "server output full revokes binding while slow socket remains open; re-login restores seq3 at(50,0), enters seq4 and moves seq5 at(60,0); prior binding fenced; other shard move, heartbeat and training succeed");
    RecordProperty(
        "fault_matrix_verification",
        "SO_RCVBUF_before_TLS+no_reads+server_4KiB_output_rejection+server_revocation+relogin");
}
}  // namespace
}  // namespace realm::game::realm
