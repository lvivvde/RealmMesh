#include "realmmesh/game/realm/realm_chat.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <map>
#include <thread>
#include "realmmesh/game/chat/chat_channels.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/network/transport/transport_factory.hpp"
#include "realmmesh/observability/metrics_registry.hpp"
namespace realm::game::realm {
namespace {
using namespace std::chrono_literals;
namespace wire = ::realmmesh::protocol::chat::v1;
class Outbox : public RealmOutbox {
public:
    std::vector<std::pair<edge::EdgeConnectionId, std::vector<std::byte>>>
        messages;
    void send(
        edge::EdgeConnectionId id, std::vector<std::byte> bytes) override {
        messages.emplace_back(id, std::move(bytes));
    }
    void close(edge::EdgeConnectionId) override {}
};
class RealmChatTest : public ::testing::Test {
protected:
    std::string cert(const char* file) {
        return (std::filesystem::path(REALMMESH_TEST_SERVICE_TLS_DIRECTORY) /
                file)
            .string();
    }
    void SetUp() override {
        network::TransportConfig tls;
        tls.name = "chat";
        tls.listen_address = "127.0.0.1";
        network::TransportConfig::TlsServerIdentity identity;
        identity.certificate_chain_file = cert("chat.pem");
        identity.private_key_file = cert("chat.key");
        identity.client_ca_file = cert("ca.pem");
        identity.allowed_client_identities = {"realm-dev-01.realm.service"};
        identity.alpn = common::chat_internal_alpn;
        tls.tls = identity;
        auto servers =
            network::TransportFactory::create_enabled(std::vector{tls});
        server_ = std::move(servers.front());
        config_.chat.enabled = true;
        config_.chat.ca_file = cert("ca.pem");
        config_.chat.certificate_file = cert("realm.pem");
        config_.chat.private_key_file = cert("realm.key");
        RealmConfig::ChatTarget target;
        target.port = server_->local_endpoint().port;
        config_.chat.targets = {target};
        thread_ = std::jthread([this](std::stop_token stop) {
            chat::ChatConfig config;
            config.channels = {0, 1, 2};
            chat::ChatChannels channels(config, "chat-dev-01", "boot");
            while (!stop.stop_requested()) {
                if (close_link_.exchange(false)) {
                    static_cast<void>(server_->close(last_peer_));
                    closed_link_ = true;
                }
                for (const auto& event : server_->poll_once(1ms)) {
                    if (event.kind ==
                        network::TransportEventKind::SessionOpened)
                        static_cast<void>(channels.opened(event.session_id));
                    else if (
                        event.kind ==
                        network::TransportEventKind::SessionClosed)
                        channels.closed(event.session_id);
                    else if (
                        event.kind ==
                        network::TransportEventKind::MessageReceived) {
                        const auto req =
                            common::decode_chat_internal_request(event.payload);
                        if (req) {
                            last_peer_ = event.session_id;
                            if (req->operation() == wire::OPERATION_JOIN)
                                ++joins_;
                            static_cast<void>(channels.submit(
                                event.session_id,
                                *req,
                                *common::chat_internal_request_id(
                                    event.payload),
                                chat::ChatChannels::Clock::now()));
                        }
                    }
                }
                channels.run_round(chat::ChatChannels::Clock::now());
                while (auto reply = channels.take_reply()) {
                    if (reply->message.operation() == wire::OPERATION_JOIN &&
                        drop_join_.exchange(false))
                        continue;
                    if (reply->message.operation() == wire::OPERATION_SEND &&
                        drop_send_.exchange(false))
                        continue;
                    if (reply->message.operation() == wire::OPERATION_SEND &&
                        blackhole_send_)
                        continue;
                    static_cast<void>(server_->send(
                        reply->peer,
                        common::encode(reply->message, reply->request_id)));
                    if (reply->message.operation() == wire::OPERATION_SEND)
                        ++send_replies_;
                }
                for (const auto& push : channels.drain_pushes())
                    static_cast<void>(server_->send(push.peer, push.payload));
            }
        });
        chat_ = std::make_unique<RealmChat>(outbox_, config_, metrics_);
    }
    void TearDown() override {
        chat_.reset();
        thread_.request_stop();
        thread_.join();
        server_.reset();
    }
    template <typename Predicate>
    bool until(Predicate predicate, std::chrono::milliseconds budget = 3s) {
        const auto deadline = std::chrono::steady_clock::now() + budget;
        while (std::chrono::steady_clock::now() < deadline) {
            chat_->complete(64);
            if (predicate()) return true;
            std::this_thread::sleep_for(2ms);
        }
        return false;
    }
    common::ChatState state(std::uint64_t session) {
        common::GetChatState query;
        query.set_kind(wire::CHANNEL_KIND_REALM);
        for (int attempt = 0; attempt < 150; ++attempt) {
            const auto id = ++request_;
            static_cast<void>(
                chat_->handle({session}, common::encode(query, id)));
            std::optional<common::ChatState> value;
            until(
                [&] {
                    for (const auto& [target, bytes] : outbox_.messages)
                        if (target.value == session &&
                            common::chat_request_id(bytes) == id)
                            value = common::decode_chat_state(bytes);
                    return value.has_value();
                },
                30ms);
            if (value && value->phase() == wire::PHASE_ACTIVE) return *value;
            std::this_thread::sleep_for(5ms);
        }
        ADD_FAILURE() << "Chat not active";
        return {};
    }
    common::SendChat send(
        const common::ChatState& state,
        std::uint64_t seq,
        std::string text = "message") {
        common::SendChat value;
        value.set_kind(state.kind());
        value.set_membership_generation(state.membership_generation());
        *value.mutable_expected_epoch() = state.epoch();
        value.set_client_send_seq(seq);
        value.set_text(std::move(text));
        return value;
    }
    common::ChatSendResult perform(
        std::uint64_t session, const common::SendChat& send) {
        const auto request = ++request_;
        EXPECT_TRUE(chat_->handle({session}, common::encode(send, request)));
        std::optional<common::ChatSendResult> value;
        EXPECT_TRUE(until([&] {
            for (const auto& [target, bytes] : outbox_.messages)
                if (target.value == session &&
                    common::chat_request_id(bytes) == request)
                    value = common::decode_chat_send_result(bytes);
            return value.has_value();
        }));
        return value.value_or(common::ChatSendResult{});
    }
    std::size_t pushes(std::uint64_t id) {
        std::size_t count = 0;
        for (const auto& [target, bytes] : outbox_.messages)
            if (target.value == id && common::decode_chat_message(bytes))
                ++count;
        return count;
    }
    RealmConfig config_;
    Outbox outbox_;
    observability::MetricsRegistry metrics_;
    std::unique_ptr<network::IMessageTransport> server_;
    std::atomic_bool drop_join_{false}, drop_send_{false},
        blackhole_send_{false};
    std::atomic_size_t joins_{0}, send_replies_{0};
    std::atomic_bool close_link_{false}, closed_link_{false};
    network::SessionId last_peer_{
        network::invalid_session_id};  // 仅测试服务线程访问。
    std::jthread thread_;
    std::unique_ptr<RealmChat> chat_;
    std::uint64_t request_{100};
};
TEST_F(
    RealmChatTest,
    AcceptedReceiptDoesNotReplaceSenderOrRecipientPushAndReplayIsUnique) {
    chat_->selected({1}, 11);
    chat_->selected({2}, 22);
    const auto a = state(1);
    const auto b = state(2);
    EXPECT_LT(a.membership_generation(), b.membership_generation());
    const auto request = send(a, 1);
    const auto receipt = perform(1, request);
    ASSERT_EQ(receipt.outcome(), wire::OUTCOME_ACCEPTED);
    ASSERT_TRUE(until([&] {
        return pushes(1) == 1 && pushes(2) == 1;
    }));
    const auto replay = perform(1, request);
    EXPECT_EQ(
        replay.receipt().SerializeAsString(),
        receipt.receipt().SerializeAsString());
    EXPECT_EQ(pushes(1), 1);
    EXPECT_EQ(pushes(2), 1);
    EXPECT_EQ(
        perform(1, send(a, 1, "conflict")).reason(),
        wire::REASON_SEQUENCE_CONFLICT);
}
TEST_F(
    RealmChatTest,
    LostJoinReplyRetainsOriginalWatermarkAndBuffersInterveningMessage) {
    chat_->selected({1}, 11);
    const auto a = state(1);
    drop_join_ = true;
    const auto before = joins_.load();
    chat_->selected({2}, 22);
    ASSERT_TRUE(until([&] {
        return joins_.load() > before;
    }));
    ASSERT_EQ(perform(1, send(a, 1)).outcome(), wire::OUTCOME_ACCEPTED);
    const auto b = state(2);
    EXPECT_EQ(b.join_watermark(), 0);
    ASSERT_TRUE(until([&] {
        return pushes(2) == 1;
    }));
}
TEST_F(
    RealmChatTest,
    LostSendReplyRetriesOriginalRequestWithoutRepeatingBroadcast) {
    chat_->selected({1}, 11);
    const auto a = state(1);
    drop_send_ = true;
    EXPECT_EQ(perform(1, send(a, 1)).outcome(), wire::OUTCOME_ACCEPTED);
    ASSERT_TRUE(until([&] {
        return pushes(1) == 1;
    }));
    EXPECT_EQ(state(1).last_client_send_seq(), 1);
    EXPECT_EQ(pushes(1), 1);
}
TEST_F(
    RealmChatTest,
    DispatchedDeadlineRemainsUnknownAndNewLinkUsesFreshQualification) {
    chat_->selected({1}, 11);
    const auto a = state(1);
    blackhole_send_ = true;
    const auto result = perform(1, send(a, 1));
    EXPECT_EQ(result.outcome(), wire::OUTCOME_UNKNOWN);
    blackhole_send_ = false;
    const auto fresh = state(1);
    EXPECT_GT(fresh.membership_generation(), a.membership_generation());
    EXPECT_EQ(fresh.join_watermark(), 1);
    EXPECT_EQ(fresh.last_client_send_seq(), 0);
    EXPECT_EQ(perform(1, send(a, 1)).outcome(), wire::OUTCOME_REJECTED);
}
TEST_F(RealmChatTest, AcceptedReplySurvivesLinkClosingBeforeFrameCollectsIt) {
    chat_->selected({1}, 11);
    const auto a = state(1);
    const auto request = ++request_;
    ASSERT_TRUE(chat_->handle({1}, common::encode(send(a, 1), request)));
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    while (send_replies_ == 0 && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_EQ(send_replies_, 1);
    // 暂停帧收完成：让 IO 收取成功回执，然后再观察服务器主动断链。
    std::this_thread::sleep_for(50ms);
    close_link_ = true;
    while (!closed_link_ && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(1ms);
    ASSERT_TRUE(closed_link_);
    std::this_thread::sleep_for(50ms);
    std::optional<common::ChatSendResult> value;
    ASSERT_TRUE(until([&] {
        for (const auto& [target, bytes] : outbox_.messages)
            if (target.value == 1 && common::chat_request_id(bytes) == request)
                value = common::decode_chat_send_result(bytes);
        return value.has_value();
    }));
    EXPECT_EQ(value->outcome(), wire::OUTCOME_ACCEPTED);
}
TEST_F(
    RealmChatTest, DisplacementDefersNewQualificationsUntilOldLeaveHasSettled) {
    chat_->selected({1}, 11);
    const auto old = state(1);
    chat_->closed({1});
    chat_->selected({2}, 11);
    const auto fresh = state(2);
    EXPECT_GT(fresh.membership_generation(), old.membership_generation());
    ASSERT_EQ(perform(2, send(fresh, 1)).outcome(), wire::OUTCOME_ACCEPTED);
    EXPECT_TRUE(until([&] {
        return pushes(2) == 1;
    }));
    EXPECT_EQ(pushes(1), 0);
}
TEST_F(
    RealmChatTest,
    LastAcceptedReplayBypassesRateBudgetAfterRejectedDifferentRequest) {
    chat_->selected({1}, 11);
    const auto a = state(1);
    const auto original = send(a, 1);
    const auto receipt = perform(1, original);
    ASSERT_EQ(receipt.outcome(), wire::OUTCOME_ACCEPTED);
    bool exhausted = false;
    for (int i = 0; i < 64; ++i) {
        const auto rejected =
            perform(1, send(a, 3, "invalid gap " + std::to_string(i)));
        if (rejected.reason() == wire::REASON_RATE_LIMITED) {
            exhausted = true;
            break;
        }
    }
    ASSERT_TRUE(exhausted);
    const auto replay = perform(1, original);
    ASSERT_EQ(replay.outcome(), wire::OUTCOME_ACCEPTED);
    EXPECT_EQ(
        replay.receipt().SerializeAsString(),
        receipt.receipt().SerializeAsString());
}
TEST_F(RealmChatTest, RapidDisplacementKeepsCharacterRateBudget) {
    chat_->selected({1}, 11);
    const auto a = state(1);
    const auto start = std::chrono::steady_clock::now();
    std::size_t accepted = 0;
    for (std::uint64_t seq = 1; seq <= 10; ++seq)
        if (perform(1, send(a, seq)).outcome() == wire::OUTCOME_ACCEPTED)
            ++accepted;
    ASSERT_EQ(accepted, 10);
    chat_->closed({1});
    chat_->selected({2}, 11);
    const auto fresh = state(2);
    std::uint64_t seq = 1;
    for (int i = 0; i < 6; ++i) {
        if (perform(2, send(fresh, seq)).outcome() == wire::OUTCOME_ACCEPTED) {
            ++accepted;
            ++seq;
        }
    }
    const auto seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start)
            .count();
    EXPECT_LE(
        static_cast<double>(accepted),
        common::chat_send_burst + seconds * common::chat_send_rate_per_second);
}
TEST_F(
    RealmChatTest,
    MissingAuthenticationDoesNotBlockFrameAndReportsUnavailable) {
    chat_.reset();
    config_.chat.ca_file.clear();
    chat_ = std::make_unique<RealmChat>(outbox_, config_, metrics_);
    const auto start = std::chrono::steady_clock::now();
    chat_->selected({1}, 11);
    chat_->complete(64);
    EXPECT_LT(std::chrono::steady_clock::now() - start, 50ms);
    common::GetChatState query;
    query.set_kind(wire::CHANNEL_KIND_REALM);
    EXPECT_TRUE(chat_->handle({1}, common::encode(query, 1)));
    ASSERT_FALSE(outbox_.messages.empty());
    EXPECT_EQ(
        common::decode_chat_state(outbox_.messages.back().second)->phase(),
        wire::PHASE_UNAVAILABLE);
}
}  // namespace
}  // namespace realm::game::realm
