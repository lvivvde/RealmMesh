#include "realmmesh/game/client/scene_session.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <thread>
#include "realmmesh/game/client/wire_login_transport.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"

namespace realm::game::client {
namespace {
using namespace std::chrono_literals;
class SceneSessionTest : public ::testing::Test {
protected:
    void SetUp() override {
        network::TransportConfig config;
        config.name = "realm";
        config.listen_address = "127.0.0.1";
        config.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = REALMMESH_TEST_TLS_CERTIFICATE,
            .private_key_file = REALMMESH_TEST_TLS_PRIVATE_KEY};
        auto transports =
            network::TransportFactory::create_enabled(std::vector{config});
        server_ = std::move(transports.front());
        port_ = server_->local_endpoint().port;
        server_thread_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested()) {
                for (const auto& event : server_->poll_once(1ms)) {
                    if (event.kind !=
                        network::TransportEventKind::MessageReceived)
                        continue;
                    const auto id = common::edge_request_id(event.payload);
                    if (!id || !common::decode_heartbeat_request(event.payload))
                        continue;
                    const auto count = burst_.exchange(0);
                    for (int i = 0; i < count; ++i) {
                        common::SceneEvent notice;
                        notice.set_shard_id(1);
                        notice.set_shard_incarnation("boot");
                        notice.set_event_seq(static_cast<std::uint64_t>(i + 1));
                        notice.set_kind(common::SceneEvent::KIND_MOVE);
                        notice.mutable_member()->set_character_id(10);
                        notice.mutable_member()->set_binding_epoch(1);
                        notice.mutable_member()
                            ->mutable_location()
                            ->set_shard_id(1);
                        static_cast<void>(server_->send(
                            event.session_id, common::encode(notice)));
                    }
                    static_cast<void>(server_->send(
                        event.session_id,
                        common::encode(common::HeartbeatResponse{}, *id)));
                }
            }
        });
    }
    void TearDown() override {
        server_thread_.request_stop();
        server_thread_.join();
        server_.reset();
    }
    std::unique_ptr<RealmSession> connect() {
        WireEndpoints endpoints;
        endpoints.verify_peer = false;
        WireLoginTransport transport(endpoints, redeemer_);
        const std::vector<network::client::EndpointCandidate> candidates{
            {.protocol = network::TransportProtocol::TlsTcp,
             .host = "127.0.0.1",
             .port = port_,
             .priority = 1}};
        auto result = transport.connect_realm(candidates, Clock::now() + 2s);
        if (!result.status.ok) {
            ADD_FAILURE() << result.status.detail;
            return nullptr;
        }
        return std::move(result.value);
    }
    WireEnterRealmRedeemer redeemer_;
    std::unique_ptr<network::IMessageTransport> server_;
    std::uint16_t port_{0};
    std::atomic_int burst_{0};
    std::jthread server_thread_;
};
TEST_F(SceneSessionTest, HeartbeatKeepsInterleavedSceneNotificationsInOrder) {
    auto session = connect();
    ASSERT_NE(session, nullptr);
    burst_ = 2;
    ASSERT_TRUE(session->heartbeat(Clock::now() + 2s));
    for (std::uint64_t seq = 1; seq <= 2; ++seq) {
        const auto notice = session->scene()->poll_scene(Clock::now() + 1s);
        ASSERT_TRUE(notice);
        ASSERT_TRUE(std::holds_alternative<common::SceneEvent>(*notice));
        EXPECT_EQ(std::get<common::SceneEvent>(*notice).event_seq(), seq);
    }
}
TEST_F(
    SceneSessionTest,
    NotificationOverflowClosesOnlySlowSessionWithoutDereferencingReleasedStream) {
    auto slow = connect();
    ASSERT_NE(slow, nullptr);
    burst_ = 257;
    EXPECT_FALSE(slow->heartbeat(Clock::now() + 2s));
    EXPECT_FALSE(slow->heartbeat(Clock::now() + 1s));
    EXPECT_FALSE(slow->scene()->poll_scene(Clock::now() + 1s));
    auto healthy = connect();
    ASSERT_NE(healthy, nullptr);
    EXPECT_TRUE(healthy->heartbeat(Clock::now() + 2s));
}
}  // namespace
}  // namespace realm::game::client
