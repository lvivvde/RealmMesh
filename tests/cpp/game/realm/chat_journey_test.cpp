#include <httplib.h>
#include "journey_fixture.hpp"
#include "realmmesh/game/client/chat_session.hpp"
#include "realmmesh/game/client/scene_session.hpp"
namespace realm::game::realm {
namespace {
namespace wire = ::realmmesh::protocol::chat::v1;
class ChatJourneyTest : public RealmJourneyTest {
protected:
    void prepare_services(const std::string& database) override {
        database_ = database;
        const auto tls =
            std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY);
        for (const auto& [env, file] :
             std::vector<std::pair<const char*, const char*>>{
                 {"REALMMESH_SERVICE_CA_FILE", "ca.pem"},
                 {"REALMMESH_REALM_CERTIFICATE_FILE", "realm.pem"},
                 {"REALMMESH_REALM_PRIVATE_KEY_FILE", "realm.key"},
                 {"REALMMESH_CHAT_CERTIFICATE_FILE", "chat.pem"},
                 {"REALMMESH_CHAT_PRIVATE_KEY_FILE", "chat.key"},
                 {"REALMMESH_SCENE_CERTIFICATE_FILE", "scene.pem"},
                 {"REALMMESH_SCENE_PRIVATE_KEY_FILE", "scene.key"}})
            set_environment(env, (tls / file).string());
        const auto ports = test_support::unused_loopback_ports(4);
        chat_port_ = ports[0];
        scene_port_ = ports[1];
        chat_metrics_ = ports[2];
        realm_metrics_ = ports[3];
        copy_configs(
            chat_root_.path(), "return { services = {{name = 'chat'}} }");
        copy_configs(
            scene_root_.path(), "return { services = {{name = 'scene'}} }");
        rewrite(
            chat_root_.path() / "services/chat.lua",
            "listen_port = 7300",
            "listen_port = " + std::to_string(chat_port_));
        rewrite(
            chat_root_.path() / "services/chat.lua",
            "metrics_port = 9107",
            "metrics_port = " + std::to_string(chat_metrics_));
        rewrite(
            scene_root_.path() / "services/scene.lua",
            "listen_port = 7200",
            "listen_port = " + std::to_string(scene_port_));
        rewrite(
            scene_root_.path() / "services/scene.lua",
            "metrics_port = 9106",
            "metrics_port = 0");
        const auto realm_config = realm_root_.path() / "services/realm.lua";
        rewrite(
            realm_config,
            "chat = {\n            enabled = false",
            "chat = {\n            enabled = true");
        rewrite(
            realm_config,
            "scene = {\n            enabled = false",
            "scene = {\n            enabled = true");
        // Chat 端点仅从真实 etcd 的精确实例获得，静态后备端口不可用。
        rewrite(realm_config, "port = 7300", "port = 1");
        rewrite(realm_config, "port = 7200", "port = 1");
        rewrite(
            realm_config,
            "metrics_port = 0",
            "metrics_port = " + std::to_string(realm_metrics_));
        for (const auto& root :
             {chat_root_.path(), scene_root_.path(), realm_root_.path()}) {
            rewrite(
                root / "common/discovery.lua",
                "enabled = false",
                "enabled = true");
        }
        for (const auto& root : {chat_root_.path(), scene_root_.path()})
            rewrite(
                root / "common/discovery.lua",
                "http://127.0.0.1:2379",
                etcd_.endpoint());
        common::MongoPlayerDataStore provision(player_data().uri(), database);
        provision.provision_account(
            {.account_id = 7002,
             .account_name = "chat-second",
             .credential = "chat-second-secret",
             .whitelisted = true});
        chat_process_.emplace(chat_root_.path());
        scene_process_.emplace(scene_root_.path());
        wait_for_port(chat_port_, "chat");
        wait_for_port(scene_port_, "scene");
    }
    void TearDown() override {
        RealmJourneyTest::TearDown();
        chat_process_.reset();
        scene_process_.reset();
        EXPECT_FALSE(test_support::loopback_port_open(chat_port_));
        EXPECT_FALSE(test_support::loopback_port_open(scene_port_));
        EXPECT_FALSE(test_support::loopback_port_open(ports_.realm));
        RecordProperty("services_and_ports_reaped", "true");
        for (const auto* env :
             {"REALMMESH_SERVICE_CA_FILE",
              "REALMMESH_REALM_CERTIFICATE_FILE",
              "REALMMESH_REALM_PRIVATE_KEY_FILE",
              "REALMMESH_CHAT_CERTIFICATE_FILE",
              "REALMMESH_CHAT_PRIVATE_KEY_FILE",
              "REALMMESH_SCENE_CERTIFICATE_FILE",
              "REALMMESH_SCENE_PRIVATE_KEY_FILE"})
            ::unsetenv(env);
    }
    common::ChatState active(
        client::RealmSession& session, wire::ChannelKind kind) {
        const auto deadline = client::Clock::now() + 5s;
        while (client::Clock::now() < deadline) {
            const auto reply = session.chat()->get_chat_state(kind, soon());
            if (reply.ok() && reply.value.phase() == wire::PHASE_ACTIVE)
                return reply.value;
            std::this_thread::sleep_for(10ms);
        }
        ADD_FAILURE() << "Chat did not become active";
        return {};
    }
    common::SendChat send(
        const common::ChatState& state,
        std::uint64_t seq,
        std::string text = "hello") {
        common::SendChat request;
        request.set_kind(state.kind());
        request.set_membership_generation(state.membership_generation());
        *request.mutable_expected_epoch() = state.epoch();
        request.set_client_send_seq(seq);
        request.set_text(std::move(text));
        return request;
    }
    std::unique_ptr<client::RealmSession> selected(
        std::string_view account = journey_account,
        std::string_view credential = journey_credential) {
        auto session = login(account, credential);
        if (!session) return {};
        const auto character = session->create_character(account, soon());
        EXPECT_TRUE(character.ok());
        const auto selection =
            session->select_character(character.value.character_id, soon());
        EXPECT_TRUE(selection.ok());
        return session;
    }
    std::vector<common::ChatMessage> messages(
        client::RealmSession& session, std::size_t count) {
        std::vector<common::ChatMessage> values;
        const auto deadline = client::Clock::now() + 3s;
        while (values.size() < count && client::Clock::now() < deadline) {
            const auto notice = session.chat()->poll_chat(deadline);
            if (!notice) break;
            if (const auto* m = std::get_if<common::ChatMessage>(&*notice))
                values.push_back(*m);
        }
        return values;
    }
    test_support::TemporaryDirectory chat_root_{"chat-journey-chat-"},
        scene_root_{"chat-journey-scene-"};
    std::optional<MeshProcess> chat_process_, scene_process_;
    std::uint16_t chat_port_{0}, scene_port_{0}, chat_metrics_{0},
        realm_metrics_{0};
    std::string database_;
};
TEST_F(
    ChatJourneyTest,
    LoginSelectionAndTwoMembersObserveContinuousChatWithTrainingHeartbeatAndScene) {
    auto first = login();
    ASSERT_TRUE(first);
    ASSERT_NE(first->chat(), nullptr);
    const auto denied =
        first->chat()->get_chat_state(wire::CHANNEL_KIND_REALM, soon());
    EXPECT_EQ(denied.status, RealmCallStatus::Rejected);
    EXPECT_EQ(denied.error_code, common::chat_error_character_not_selected);
    auto created = first->create_character("ChatFirst", soon());
    ASSERT_TRUE(created.ok());
    ASSERT_TRUE(
        first->select_character(created.value.character_id, soon()).ok());
    auto second = selected("chat-second", "chat-second-secret");
    ASSERT_TRUE(second);
    const auto a = active(*first, wire::CHANNEL_KIND_REALM);
    const auto b = active(*second, wire::CHANNEL_KIND_REALM);
    EXPECT_NE(a.membership_generation(), b.membership_generation());
    const auto forged =
        first->chat()->send_chat(send(b, 1, "wrong qualification"), soon());
    ASSERT_TRUE(forged.ok());
    EXPECT_EQ(forged.value.outcome(), wire::OUTCOME_REJECTED);
    const auto request = send(a, 1);
    const auto result = first->chat()->send_chat(request, soon());
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value.outcome(), wire::OUTCOME_ACCEPTED);
    ASSERT_TRUE(first->train(1, soon()).ok());
    ASSERT_TRUE(second->heartbeat(soon()));
    const auto second_result =
        second->chat()->send_chat(send(b, 1, "second"), soon());
    ASSERT_TRUE(second_result.ok());
    ASSERT_EQ(second_result.value.outcome(), wire::OUTCOME_ACCEPTED);
    const auto replay = first->chat()->send_chat(request, soon());
    ASSERT_TRUE(replay.ok());
    EXPECT_EQ(
        replay.value.receipt().SerializeAsString(),
        result.value.receipt().SerializeAsString());
    const auto left = messages(*first, 2), right = messages(*second, 2);
    ASSERT_EQ(left.size(), 2);
    ASSERT_EQ(right.size(), 2);
    for (std::size_t i = 0; i < 2; ++i) {
        EXPECT_EQ(left[i].channel_seq(), i + 1);
        EXPECT_EQ(right[i].channel_seq(), i + 1);
        EXPECT_EQ(
            left[i].sender_character_id(), right[i].sender_character_id());
    }
    EXPECT_EQ(left[0].sender_character_id(), created.value.character_id);
    ASSERT_TRUE(first->scene()->enter_scene(1, soon()).ok());
    ASSERT_TRUE(second->scene()->enter_scene(1, soon()).ok());
    const auto scene_a = active(*first, wire::CHANNEL_KIND_SCENE);
    static_cast<void>(active(*second, wire::CHANNEL_KIND_SCENE));
    const auto scene_send =
        first->chat()->send_chat(send(scene_a, 1, "scene"), soon());
    ASSERT_TRUE(scene_send.ok());
    EXPECT_EQ(scene_send.value.outcome(), wire::OUTCOME_ACCEPTED);
    ASSERT_TRUE(first->scene()->move_in_scene(2, 4, 5, soon()).ok());
    ASSERT_TRUE(first->heartbeat(soon()));
    ASSERT_EQ(messages(*first, 1).size(), 1);
    ASSERT_EQ(messages(*second, 1).size(), 1);
    common::MongoPlayerDataStore store(player_data().uri(), database_);
    const auto record =
        store.scene_state({journey_account_id, 1, created.value.character_id});
    ASSERT_TRUE(record);
    ASSERT_TRUE(record->confirmed_location);
    EXPECT_EQ(record->confirmed_location->x, 4);
    RecordProperty("database_scene_location_verified", "true");
    for (const auto port : {chat_metrics_, realm_metrics_}) {
        httplib::Client http("127.0.0.1", port);
        const auto response = http.Get("/metrics");
        ASSERT_TRUE(response);
        EXPECT_EQ(response->status, 200);
        EXPECT_NE(
            response->body.find(
                port == chat_metrics_ ? "chat_send_total"
                                      : "realm_chat_send_total"),
            std::string::npos);
    }
    RecordProperty("chat_and_realm_http_metrics_verified", "true");
}
TEST_F(
    ChatJourneyTest,
    ChatCrashAndSameNameRestartDoNotBlockOtherRealmOperationsOrReplayOldUnknown) {
    auto session = selected();
    ASSERT_TRUE(session);
    const auto before = active(*session, wire::CHANNEL_KIND_REALM);
    ASSERT_TRUE(session->scene()->enter_scene(1, soon()).ok());
    chat_process_->crash();
    chat_process_.reset();
    EXPECT_FALSE(test_support::loopback_port_open(chat_port_));
    ASSERT_TRUE(session->heartbeat(soon()));
    ASSERT_TRUE(session->train(1, soon()).ok());
    ASSERT_TRUE(session->scene()->move_in_scene(2, 6, 7, soon()).ok());
    const auto failed = session->chat()->send_chat(send(before, 1), soon());
    ASSERT_TRUE(failed.ok());
    EXPECT_NE(failed.value.outcome(), wire::OUTCOME_ACCEPTED);
    cluster::EtcdRegistryOptions options;
    options.endpoint = etcd_.endpoint();
    options.background_maintenance = false;
    cluster::EtcdServiceRegistry registry(options);
    const auto expires = client::Clock::now() + 10s;
    while (!registry.discover("chat").empty() && client::Clock::now() < expires)
        std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(registry.discover("chat").empty());
    chat_process_.emplace(chat_root_.path());
    wait_for_port(chat_port_, "restarted chat");
    const auto after = active(*session, wire::CHANNEL_KIND_REALM);
    EXPECT_NE(
        before.epoch().chat_incarnation(), after.epoch().chat_incarnation());
    EXPECT_GT(after.membership_generation(), before.membership_generation());
    EXPECT_EQ(after.join_watermark(), 0);
    const auto accepted = session->chat()->send_chat(
        send(after, 1, "explicit new message"), soon());
    ASSERT_TRUE(accepted.ok());
    EXPECT_EQ(accepted.value.outcome(), wire::OUTCOME_ACCEPTED);
    ASSERT_EQ(messages(*session, 1).size(), 1);
    ASSERT_TRUE(session->train(2, soon()).ok());
    ASSERT_TRUE(session->heartbeat(soon()));
}
}  // namespace
}  // namespace realm::game::realm
