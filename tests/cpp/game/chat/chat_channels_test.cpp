#include "chat_test_messages.hpp"
#include <gtest/gtest.h>
#include <chrono>
#include <limits>
using namespace std::chrono_literals;
namespace realm::game::chat {
namespace {
using namespace test;
TEST(ChatChannelsTest, SameChannelOrdersAllSendersAndFansOutIncludingSender) {
    World w; const auto a = member(1), b = member(2, 8);
    ASSERT_EQ(w.join(a).status(), wire::STATUS_OK); ASSERT_EQ(w.join(b).status(), wire::STATUS_OK);
    EXPECT_EQ(w.call(send(w.link, a)).result().receipt().channel_seq(), 1);
    EXPECT_EQ(w.call(send(w.link, b)).result().receipt().channel_seq(), 2);
    const auto messages = w.drain(); ASSERT_EQ(messages.size(), 4);
    for (std::size_t i = 0; i < messages.size(); ++i) EXPECT_EQ(messages[i].message().channel_seq(), i / 2 + 1);
    EXPECT_EQ(messages[0].message().sender_character_id(), 7);
    EXPECT_EQ(messages[2].message().sender_character_id(), 8);
    EXPECT_EQ(w.channels.pending_push_bytes(), 0);
}
TEST(ChatChannelsTest, LostJoinResponseRetryKeepsOriginalWatermarkAndLastReceipt) {
    World w; const auto a = member(1), b = member(2, 8);
    ASSERT_EQ(w.join(a).state().join_watermark(), 0); ASSERT_EQ(w.join(b).status(), wire::STATUS_OK);
    ASSERT_EQ(w.call(send(w.link, b)).result().outcome(), wire::OUTCOME_ACCEPTED);
    EXPECT_EQ(w.join(a).state().join_watermark(), 0);
    ASSERT_EQ(w.call(send(w.link, a)).result().receipt().channel_seq(), 2);
    const auto retry = w.join(a);
    EXPECT_EQ(retry.state().join_watermark(), 0); EXPECT_EQ(retry.state().last_client_send_seq(), 1);
    EXPECT_EQ(retry.state().last_receipt().channel_seq(), 2);
    EXPECT_EQ(w.call(send(w.link, a, 2)).result().receipt().channel_seq(), 3);
}
TEST(ChatChannelsTest, LeaveBeforeJoinAndOldCleanupNeverResurrectOrDeleteReplacement) {
    World w; const auto old = member(1), replacement = member(2);
    ASSERT_EQ(w.join(old).status(), wire::STATUS_OK); ASSERT_EQ(w.join(replacement).status(), wire::STATUS_OK);
    EXPECT_EQ(w.call(request(w.link, wire::OPERATION_LEAVE, old)).status(), wire::STATUS_OK);
    EXPECT_EQ(w.join(old).status(), wire::STATUS_STALE_MEMBERSHIP);
    EXPECT_EQ(w.call(send(w.link, replacement)).result().outcome(), wire::OUTCOME_ACCEPTED);
    const auto never_joined = member(3, 9);
    EXPECT_EQ(w.call(request(w.link, wire::OPERATION_LEAVE, never_joined)).status(), wire::STATUS_OK);
    EXPECT_EQ(w.join(never_joined).status(), wire::STATUS_STALE_MEMBERSHIP);
    EXPECT_EQ(w.join(replacement).state().last_client_send_seq(), 1);
    EXPECT_EQ(w.channels.active_members(), 1);
}
TEST(ChatChannelsTest, RetryReplaysOnceAndConflictOldOrGapDoesNotConsumeSequence) {
    World w; const auto a = member(1); ASSERT_EQ(w.join(a).status(), wire::STATUS_OK);
    const auto first = w.call(send(w.link, a)); ASSERT_EQ(w.drain().size(), 1);
    EXPECT_EQ(w.call(send(w.link, a)).result().receipt().SerializeAsString(), first.result().receipt().SerializeAsString());
    EXPECT_TRUE(w.drain().empty());
    EXPECT_EQ(w.call(send(w.link, a, 1, "冲突")).result().reason(), wire::REASON_SEQUENCE_CONFLICT);
    EXPECT_EQ(w.call(send(w.link, a, 3)).result().reason(), wire::REASON_INVALID_SEQUENCE);
    EXPECT_EQ(w.call(send(w.link, a, 2)).result().receipt().channel_seq(), 2);
    EXPECT_EQ(w.call(send(w.link, a, 1)).result().reason(), wire::REASON_INVALID_SEQUENCE);
    EXPECT_EQ(w.channels.stats().replayed, 1);
}
TEST(ChatChannelsTest, NewLinkFencesOldRequestsAndLateCloseWithoutResettingChannelWatermark) {
    World w; const auto a = member(1); ASSERT_EQ(w.join(a).status(), wire::STATUS_OK);
    ASSERT_EQ(w.call(send(w.link, a)).result().receipt().channel_seq(), 1);
    const auto old = w.link; ASSERT_TRUE(w.channels.opened(2));
    w.link = w.call(hello(), 2).link();
    EXPECT_GT(w.link.link_generation(), old.link_generation());
    EXPECT_EQ(w.channels.take_closed_peers(), std::vector<ChatChannels::Peer>{1});
    w.channels.closed(1);
    EXPECT_EQ(w.call(request(w.link, wire::OPERATION_JOIN, a), 2).state().join_watermark(), 1);
    EXPECT_EQ(w.call(send(old, a), 2).status(), wire::STATUS_STALE_LINK);
    EXPECT_EQ(w.call(send(w.link, a), 2).result().receipt().channel_seq(), 2);
    EXPECT_EQ(w.channels.active_members(), 1);
}
TEST(ChatChannelsTest, FreshEpochDoesNotReplayOldReceiptAndIdleChannelRetainsWatermark) {
    World w; const auto a = member(1); ASSERT_EQ(w.join(a).status(), wire::STATUS_OK);
    ASSERT_EQ(w.call(send(w.link, a)).result().receipt().channel_seq(), 1);
    ASSERT_EQ(w.call(request(w.link, wire::OPERATION_LEAVE, a)).status(), wire::STATUS_OK);
    EXPECT_TRUE(w.drain().empty());
    EXPECT_EQ(w.join(member(2)).state().join_watermark(), 1);
    World restarted({}, "chat-run-2");
    EXPECT_EQ(restarted.call(send(w.link, member(2))).status(), wire::STATUS_STALE_LINK);
    const auto joined = restarted.join(a); EXPECT_EQ(joined.state().join_watermark(), 0);
    EXPECT_FALSE(joined.state().has_last_receipt());
}
TEST(ChatChannelsTest, RoleMembershipAndChannelCapacityBoundsAreShared) {
    World w;
    for (std::uint64_t i = 1; i <= 16; ++i) ASSERT_EQ(w.join(member(i, i)).status(), wire::STATUS_OK);
    EXPECT_EQ(w.join(member(17, 17)).status(), wire::STATUS_BUSY);
    ASSERT_EQ(w.join(member(18, 1, 0)).status(), wire::STATUS_OK);
    EXPECT_EQ(w.join(member(19, 1, 2)).status(), wire::STATUS_BUSY);
    for (std::uint64_t i = 2; i <= 64; ++i) ASSERT_EQ(w.join(member(20 + i, i, 0)).status(), wire::STATUS_OK);
    EXPECT_EQ(w.join(member(100, 65, 0)).status(), wire::STATUS_BUSY);
    EXPECT_EQ(w.join(member(101, 1, 0)).status(), wire::STATUS_OK);
}
TEST(ChatChannelsTest, ConfiguredSubsetRejectsCrossRealmAndUnownedChannel) {
    ChatConfig config; config.channels = {0, 1}; World w(config);
    EXPECT_EQ(w.join(member(1, 7, 2)).status(), wire::STATUS_FORBIDDEN);
    auto r = hello(); r.mutable_link()->set_realm_id(2);
    ASSERT_TRUE(w.channels.opened(2)); EXPECT_EQ(w.call(r, 2).status(), wire::STATUS_FORBIDDEN);
    r = hello(); r.mutable_link()->set_realm_instance_id("other");
    EXPECT_EQ(w.call(r, 2).status(), wire::STATUS_FORBIDDEN);
    r = hello(); r.mutable_link()->set_chat_instance_id("other");
    EXPECT_EQ(w.call(r, 2).status(), wire::STATUS_FORBIDDEN);
}
TEST(ChatChannelsTest, CountAndByteReservationRejectBeforeReceiptAndRecoverWithoutSequenceGap) {
    ChatConfig config; config.pending_pushes = 1; World w(config);
    const auto a = member(1), b = member(2, 8);
    ASSERT_EQ(w.join(a).status(), wire::STATUS_OK); ASSERT_EQ(w.join(b).status(), wire::STATUS_OK);
    EXPECT_EQ(w.call(send(w.link, a)).result().reason(), wire::REASON_RESOURCE_LIMIT);
    EXPECT_EQ(w.channels.pending_pushes(), 0);
    EXPECT_EQ(w.call(request(w.link, wire::OPERATION_STATE, a)).state().last_client_send_seq(), 0);
    ASSERT_EQ(w.call(request(w.link, wire::OPERATION_LEAVE, b)).status(), wire::STATUS_OK);
    EXPECT_EQ(w.call(send(w.link, a)).result().receipt().channel_seq(), 1);
    const auto bytes = w.channels.pending_push_bytes(); ASSERT_GT(bytes, 1);
    config.pending_push_bytes = bytes - 1; World too_small(config);
    ASSERT_EQ(too_small.join(a).status(), wire::STATUS_OK);
    EXPECT_EQ(too_small.call(send(too_small.link, a)).result().reason(), wire::REASON_RESOURCE_LIMIT);
    EXPECT_EQ(too_small.channels.pending_push_bytes(), 0);
    config.pending_push_bytes = bytes; World exact(config); ASSERT_EQ(exact.join(a).status(), wire::STATUS_OK);
    EXPECT_EQ(exact.call(send(exact.link, a)).result().outcome(), wire::OUTCOME_ACCEPTED);
    EXPECT_EQ(exact.channels.pending_push_bytes(), bytes);
}
TEST(ChatChannelsTest, RateLimitSharesMembershipTokensButNeverBlocksReceiptReplay) {
    World w; const auto a = member(1); ASSERT_EQ(w.join(a).status(), wire::STATUS_OK);
    for (std::uint64_t i = 1; i <= 10; ++i) {
        ASSERT_EQ(w.call(send(w.link, a, i)).result().outcome(), wire::OUTCOME_ACCEPTED); w.drain();
    }
    EXPECT_EQ(w.call(send(w.link, a, 10)).result().outcome(), wire::OUTCOME_ACCEPTED);
    EXPECT_EQ(w.call(send(w.link, a, 11)).result().reason(), wire::REASON_RATE_LIMITED);
    w.now += 200ms;
    EXPECT_EQ(w.call(send(w.link, a, 11)).result().outcome(), wire::OUTCOME_ACCEPTED);
}
TEST(ChatChannelsTest, PendingRepliesStillOccupyBudgetAndQueueDeadlineDoesNotExecute) {
    ChatConfig config; config.pending_requests = 2; World w(config);
    const auto a = member(1); ASSERT_EQ(w.join(a).status(), wire::STATUS_OK);
    auto r = send(w.link, a);
    ASSERT_TRUE(w.channels.submit(1, r, 10, w.now)); ASSERT_TRUE(w.channels.submit(1, request(w.link, wire::OPERATION_STATE, a), 11, w.now));
    EXPECT_FALSE(w.channels.submit(1, r, 12, w.now));
    w.now += 1s; w.channels.run_round(w.now);
    EXPECT_EQ(w.channels.pending_requests(), 2); EXPECT_FALSE(w.channels.submit(1, r, 12, w.now));
    ASSERT_TRUE(w.channels.take_reply()); ASSERT_TRUE(w.channels.take_reply());
    EXPECT_EQ(w.channels.pending_pushes(), 0); EXPECT_EQ(w.channels.stats().expired, 2);
    EXPECT_EQ(w.call(r).result().receipt().channel_seq(), 1);
}
TEST(ChatChannelsTest, EachMembershipAcceptsAtMostOneSendPerRoundWithoutStarvingAnother) {
    World w; const auto a = member(1), b = member(2, 8);
    ASSERT_EQ(w.join(a).status(), wire::STATUS_OK); ASSERT_EQ(w.join(b).status(), wire::STATUS_OK);
    ASSERT_TRUE(w.channels.submit(1, send(w.link, a, 1), 10, w.now));
    EXPECT_FALSE(w.channels.submit(1, send(w.link, a, 2), 11, w.now));
    ASSERT_TRUE(w.channels.submit(1, send(w.link, b, 1), 12, w.now));
    w.channels.run_round(w.now);
    const auto first = w.channels.take_reply(), second = w.channels.take_reply();
    ASSERT_TRUE(first); ASSERT_TRUE(second); EXPECT_EQ(first->request_id, 10); EXPECT_EQ(second->request_id, 12);
    EXPECT_FALSE(w.channels.take_reply()); EXPECT_EQ(w.channels.pending_requests(), 0);
    ASSERT_TRUE(w.channels.submit(1, send(w.link, a, 2), 11, w.now));
    w.channels.run_round(w.now); const auto third = w.channels.take_reply(); ASSERT_TRUE(third);
    EXPECT_EQ(third->message.result().receipt().channel_seq(), 3);
}
TEST(ChatChannelsTest, GlobalAndPerPeerQueueAndConnectionBoundsReleaseAllOccupancy) {
    ChatChannels c({}, "chat-dev-01", "run");
    for (std::uint64_t peer = 1; peer <= 8; ++peer) {
        ASSERT_TRUE(c.opened(peer));
        for (std::uint64_t n = 1; n <= 8; ++n) ASSERT_TRUE(c.submit(peer, hello(), n));
        EXPECT_FALSE(c.submit(peer, hello(), 9));
    }
    EXPECT_EQ(c.pending_requests(), 64);
    ASSERT_TRUE(c.opened(9)); EXPECT_FALSE(c.submit(9, hello(), 1));
    for (std::uint64_t peer = 10; peer <= 16; ++peer) ASSERT_TRUE(c.opened(peer));
    EXPECT_FALSE(c.opened(17));
    for (std::uint64_t peer = 1; peer <= 16; ++peer) c.closed(peer);
    EXPECT_EQ(c.pending_requests(), 0); EXPECT_EQ(c.active_members(), 0); EXPECT_TRUE(c.opened(17));
}
TEST(ChatChannelsTest, ExhaustedGenerationsRequireFreshAuthenticatedLinkAndNeverWrap) {
    World w; const auto max = member(common::chat_counter_max);
    EXPECT_EQ(w.join(max).status(), wire::STATUS_OK); EXPECT_EQ(w.join(member(1, 8)).status(), wire::STATUS_STALE_MEMBERSHIP);
    EXPECT_EQ(w.join(max).status(), wire::STATUS_OK);
    EXPECT_EQ(ChatChannels::next_counter(0), 1); EXPECT_EQ(ChatChannels::next_counter(common::chat_counter_max - 1), common::chat_counter_max);
    EXPECT_FALSE(ChatChannels::next_counter(common::chat_counter_max));
    EXPECT_FALSE(ChatChannels::next_counter(std::numeric_limits<std::uint64_t>::max()));
    ASSERT_TRUE(w.channels.opened(2)); w.link = w.call(hello(), 2).link();
    EXPECT_EQ(w.call(request(w.link, wire::OPERATION_JOIN, member(1)), 2).status(), wire::STATUS_OK);
}
TEST(ChatChannelsTest, UnknownWireFieldsAreAcceptedButNotRetainedOrAmplifiedIntoPushes) {
    World w;
    auto a = member(1);
    auto bytes = a.SerializeAsString();
    bytes += std::string("\x9a\x06\x90\x4e", 4) + std::string(10000, 'x');
    ASSERT_TRUE(a.ParseFromString(bytes));
    const auto joined = w.join(a); ASSERT_EQ(joined.status(), wire::STATUS_OK);
    EXPECT_LT(joined.membership().ByteSizeLong(), 128);
    EXPECT_EQ(w.call(send(w.link, a)).result().outcome(), wire::OUTCOME_ACCEPTED);
    const auto pushes = w.drain(); ASSERT_EQ(pushes.size(), 1);
    EXPECT_LT(pushes.front().ByteSizeLong(), 1024);
    EXPECT_EQ(w.join(a).state().last_client_send_seq(), 1);
}
TEST(ChatChannelsTest, InvalidConfigurationRejectsUnboundedOrAmbiguousValues) {
    auto c = ChatConfig{}; c.channels = {0, 0}; EXPECT_THROW(validate_chat_config(c), std::invalid_argument);
    c = {}; c.channels = {}; EXPECT_THROW(validate_chat_config(c), std::invalid_argument);
    c = {}; c.channels = {3}; EXPECT_THROW(validate_chat_config(c), std::invalid_argument);
    c = {}; c.pending_requests = 65; EXPECT_THROW(validate_chat_config(c), std::invalid_argument);
    c = {}; c.pending_push_bytes = 0; EXPECT_THROW(validate_chat_config(c), std::invalid_argument);
    c = {}; c.queued_per_link = 9; EXPECT_THROW(validate_chat_config(c), std::invalid_argument);
    c = {}; c.operation_timeout_ms = 1001; EXPECT_THROW(validate_chat_config(c), std::invalid_argument);
}
}  // namespace
}  // namespace realm::game::chat
