#include "realmmesh/game/client/chat_session.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <thread>
#include "realmmesh/game/client/scene_session.hpp"
#include "realmmesh/game/client/wire_login_transport.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"
namespace realm::game::client {
namespace {
using namespace std::chrono_literals;
namespace wire = ::realmmesh::protocol::chat::v1;
class ChatSessionTest : public ::testing::Test {
protected:
    common::ChatState state() {
        common::ChatState s;
        s.set_kind(wire::CHANNEL_KIND_REALM);
        s.set_phase(wire::PHASE_ACTIVE);
        s.set_membership_generation(1);
        s.mutable_epoch()->set_chat_incarnation("boot");
        s.mutable_epoch()->mutable_channel()->set_realm_id(1);
        s.mutable_epoch()->mutable_channel()->set_kind(s.kind());
        return s;
    }
    common::ChatMessage message(std::uint64_t seq) {
        common::ChatMessage m;
        m.set_membership_generation(1);
        *m.mutable_epoch() = state().epoch();
        m.set_channel_seq(seq);
        m.set_sender_character_id(11);
        m.set_text("hello");
        return m;
    }
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
        thread_ = std::jthread([this](std::stop_token stop) {
            while (!stop.stop_requested())
                for (const auto& event : server_->poll_once(1ms)) {
                    if (event.kind !=
                        network::TransportEventKind::MessageReceived)
                        continue;
                    const auto send = [&](auto value, std::uint64_t id = 0) {
                        static_cast<void>(server_->send(
                            event.session_id, common::encode(value, id)));
                    };
                    if (common::decode_get_chat_state(event.payload)) {
                        auto reply = state();
                        if (query_busy_) {
                            reply.Clear();
                            reply.set_kind(wire::CHANNEL_KIND_REALM);
                            reply.set_phase(wire::PHASE_UNAVAILABLE);
                            reply.set_reason(wire::REASON_BUSY);
                        }
                        send(reply, *common::chat_request_id(event.payload));
                    } else if (
                        auto request =
                            common::decode_send_chat(event.payload)) {
                        if (drop_send_.exchange(false)) continue;
                        common::ChatSendResult result;
                        result.set_kind(request->kind());
                        result.set_membership_generation(1);
                        *result.mutable_epoch() = request->expected_epoch();
                        result.set_client_send_seq(request->client_send_seq());
                        if (reject_) {
                            result.set_outcome(wire::OUTCOME_REJECTED);
                            result.set_reason(wire::REASON_STALE_MEMBERSHIP);
                        } else {
                            result.set_outcome(wire::OUTCOME_ACCEPTED);
                            *result.mutable_receipt()->mutable_epoch() =
                                request->expected_epoch();
                            result.mutable_receipt()->set_client_send_seq(
                                request->client_send_seq());
                            result.mutable_receipt()->set_channel_seq(1);
                        }
                        send(result, *common::chat_request_id(event.payload));
                    } else if (common::decode_heartbeat_request(
                                   event.payload)) {
                        const auto count = burst_.exchange(0);
                        for (int i = 0; i < count; ++i)
                            send(message(
                                static_cast<std::uint64_t>(i + 1 + offset_)));
                        if (scene_notice_) {
                            common::SceneUnavailable m;
                            m.set_shard_id(1);
                            m.set_binding_epoch(1);
                            m.set_retry_after_seconds(1);
                            send(m);
                        }
                        send(
                            common::HeartbeatResponse{},
                            *common::edge_request_id(event.payload));
                    }
                }
        });
        session_ = connect();
        ASSERT_NE(session_, nullptr);
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
        auto connected = transport.connect_realm(candidates, Clock::now() + 2s);
        if (!connected.status.ok) {
            ADD_FAILURE() << connected.status.detail;
            return nullptr;
        }
        return std::move(connected.value);
    }
    void TearDown() override {
        session_.reset();
        thread_.request_stop();
        thread_.join();
        server_.reset();
    }
    common::SendChat send() {
        const auto value = state();
        common::SendChat s;
        s.set_kind(value.kind());
        s.set_membership_generation(1);
        *s.mutable_expected_epoch() = value.epoch();
        s.set_client_send_seq(1);
        s.set_text("hello");
        return s;
    }
    std::unique_ptr<network::IMessageTransport> server_;
    std::uint16_t port_{0};
    std::atomic_int burst_{0}, offset_{0};
    std::atomic_bool query_busy_{false}, scene_notice_{false},
        drop_send_{false}, reject_{false};
    std::jthread thread_;
    WireEnterRealmRedeemer redeemer_;
    std::unique_ptr<RealmSession> session_;
};
TEST_F(
    ChatSessionTest,
    InterleavedChatAndSceneSurviveHeartbeatBeforeInitialChatState) {
    burst_ = 2;
    scene_notice_ = true;
    ASSERT_TRUE(session_->heartbeat(Clock::now() + 2s));
    ASSERT_TRUE(
        session_->chat()
            ->get_chat_state(wire::CHANNEL_KIND_REALM, Clock::now() + 2s)
            .ok());
    for (std::uint64_t seq = 1; seq <= 2; ++seq) {
        const auto value = session_->chat()->poll_chat(Clock::now() + 1s);
        ASSERT_TRUE(value);
        ASSERT_TRUE(std::holds_alternative<common::ChatMessage>(*value));
        EXPECT_EQ(std::get<common::ChatMessage>(*value).channel_seq(), seq);
    }
    const auto scene = session_->scene()->poll_scene(Clock::now() + 1s);
    ASSERT_TRUE(scene);
    EXPECT_TRUE(std::holds_alternative<common::SceneUnavailable>(*scene));
}
TEST_F(
    ChatSessionTest,
    SenderReceiptAndRepeatedStateDoNotAdvanceNotificationWatermark) {
    ASSERT_TRUE(
        session_->chat()
            ->get_chat_state(wire::CHANNEL_KIND_REALM, Clock::now() + 2s)
            .ok());
    const auto result = session_->chat()->send_chat(send(), Clock::now() + 2s);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.value.outcome(), wire::OUTCOME_ACCEPTED);
    burst_ = 1;
    ASSERT_TRUE(session_->heartbeat(Clock::now() + 2s));
    ASSERT_TRUE(
        session_->chat()
            ->get_chat_state(wire::CHANNEL_KIND_REALM, Clock::now() + 2s)
            .ok());
    const auto value = session_->chat()->poll_chat(Clock::now() + 1s);
    ASSERT_TRUE(value);
    EXPECT_EQ(std::get<common::ChatMessage>(*value).channel_seq(), 1);
    burst_ = 1;
    ASSERT_TRUE(session_->heartbeat(Clock::now() + 2s));
    EXPECT_FALSE(session_->chat()->poll_chat(Clock::now() + 30ms));
}
TEST_F(
    ChatSessionTest, BusyStateQueryDoesNotResetExistingNotificationWatermark) {
    ASSERT_TRUE(
        session_->chat()
            ->get_chat_state(wire::CHANNEL_KIND_REALM, Clock::now() + 2s)
            .ok());
    burst_ = 1;
    ASSERT_TRUE(session_->heartbeat(Clock::now() + 2s));
    ASSERT_TRUE(session_->chat()->poll_chat(Clock::now() + 1s));
    query_busy_ = true;
    const auto busy = session_->chat()->get_chat_state(
        wire::CHANNEL_KIND_REALM, Clock::now() + 2s);
    ASSERT_TRUE(busy.ok());
    EXPECT_EQ(busy.value.reason(), wire::REASON_BUSY);
    query_busy_ = false;
    ASSERT_TRUE(
        session_->chat()
            ->get_chat_state(wire::CHANNEL_KIND_REALM, Clock::now() + 2s)
            .ok());
    offset_ = 1;
    burst_ = 1;
    ASSERT_TRUE(session_->heartbeat(Clock::now() + 2s));
    const auto value = session_->chat()->poll_chat(Clock::now() + 1s);
    ASSERT_TRUE(value);
    ASSERT_TRUE(std::holds_alternative<common::ChatMessage>(*value));
    EXPECT_EQ(std::get<common::ChatMessage>(*value).channel_seq(), 2);
}
TEST_F(
    ChatSessionTest,
    GapIsExplicitAndOverflowClosesOnlySlowSessionWhileHealthySessionContinues) {
    auto healthy = connect();
    ASSERT_NE(healthy, nullptr);
    ASSERT_TRUE(
        session_->chat()
            ->get_chat_state(wire::CHANNEL_KIND_REALM, Clock::now() + 2s)
            .ok());
    offset_ = 1;
    burst_ = 1;
    ASSERT_TRUE(session_->heartbeat(Clock::now() + 2s));
    const auto gap = session_->chat()->poll_chat(Clock::now() + 1s);
    ASSERT_TRUE(gap);
    ASSERT_TRUE(std::holds_alternative<common::ChatUnavailable>(*gap));
    EXPECT_EQ(
        std::get<common::ChatUnavailable>(*gap).reason(), wire::REASON_GAP);
    burst_ = 257;
    EXPECT_FALSE(session_->heartbeat(Clock::now() + 2s));
    EXPECT_FALSE(session_->heartbeat(Clock::now() + 30ms));

    scene_notice_ = true;
    ASSERT_TRUE(healthy->heartbeat(Clock::now() + 2s));
    const auto scene = healthy->scene()->poll_scene(Clock::now() + 1s);
    ASSERT_TRUE(scene);
    EXPECT_TRUE(std::holds_alternative<common::SceneUnavailable>(*scene));
    const auto active = healthy->chat()->get_chat_state(
        wire::CHANNEL_KIND_REALM, Clock::now() + 2s);
    ASSERT_TRUE(active.ok());
    EXPECT_EQ(active.value.phase(), wire::PHASE_ACTIVE);
    const auto sent =
        healthy->chat()->send_chat(send(), Clock::now() + 2s);
    ASSERT_TRUE(sent.ok());
    EXPECT_EQ(sent.value.outcome(), wire::OUTCOME_ACCEPTED);
    RecordProperty(
        "fault_matrix_verification",
        "reference_client_256_notification_boundary+257th_notification_closes_only_slow_Realm_Session+healthy_heartbeat_scene_and_chat_continue");
}
TEST_F(
    ChatSessionTest,
    UnknownRetryDoesNotBecomeRejectedWhenQualificationHasEnded) {
    drop_send_ = true;
    const auto unknown =
        session_->chat()->send_chat(send(), Clock::now() + 100ms);
    ASSERT_TRUE(unknown.ok());
    ASSERT_EQ(unknown.value.outcome(), wire::OUTCOME_UNKNOWN);
    reject_ = true;
    const auto retry = session_->chat()->send_chat(send(), Clock::now() + 2s);
    ASSERT_TRUE(retry.ok());
    EXPECT_EQ(retry.value.outcome(), wire::OUTCOME_UNKNOWN);
}
}  // namespace
}  // namespace realm::game::client
