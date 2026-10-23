#include "realmmesh/game/common/chat_protocol.hpp"
#include "realmmesh/common/v1/envelope.pb.h"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/realm_protocol.hpp"
#include "realmmesh/game/common/scene_protocol.hpp"
#include "realmmesh/game/common/scene_internal_protocol.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <limits>
#include <stdexcept>
#include <string>

namespace realm::game::common {
namespace {
namespace chat = ::realmmesh::protocol::chat::v1;
using Envelope = ::realmmesh::protocol::common::v1::Envelope;

std::vector<std::byte> raw(const auto& message, std::uint32_t type,
                           std::uint64_t request_id = 1, std::uint32_t version = 1) {
    Envelope envelope;
    envelope.set_protocol_version(version);
    envelope.set_message_id(type);
    envelope.set_request_id(request_id);
    envelope.set_payload(message.SerializeAsString());
    const auto serialized = envelope.SerializeAsString();
    const auto bytes = protobuf_bytes(serialized);
    return {bytes.begin(), bytes.end()};
}
chat::Channel channel(chat::ChannelKind kind = chat::CHANNEL_KIND_SCENE) {
    chat::Channel m;
    m.set_realm_id(1);
    m.set_kind(kind);
    m.set_shard_id(kind == chat::CHANNEL_KIND_SCENE ? 1 : 0);
    return m;
}
chat::Epoch epoch(chat::ChannelKind kind = chat::CHANNEL_KIND_SCENE) {
    chat::Epoch m;
    *m.mutable_channel() = channel(kind);
    m.set_chat_incarnation("chat-run-1");
    return m;
}
chat::Membership membership(chat::ChannelKind kind = chat::CHANNEL_KIND_SCENE) {
    chat::Membership m;
    *m.mutable_channel() = channel(kind);
    m.set_character_id(std::numeric_limits<std::uint64_t>::max());
    m.set_realm_incarnation("realm-run-1");
    m.set_session_binding(11);
    m.set_generation(3);
    if (kind == chat::CHANNEL_KIND_SCENE) m.set_scene_binding_epoch(7);
    return m;
}
chat::Link link(bool hello = false) {
    chat::Link m;
    m.set_realm_id(1);
    m.set_realm_instance_id("realm-a");
    m.set_realm_incarnation("realm-run-1");
    m.set_chat_instance_id("chat-a");
    if (!hello) {
        m.set_chat_incarnation("chat-run-1");
        m.set_link_generation(2);
    }
    return m;
}
chat::Receipt receipt() {
    chat::Receipt m;
    *m.mutable_epoch() = epoch();
    m.set_channel_seq(11);
    m.set_client_send_seq(1);
    return m;
}
ChatState state() {
    ChatState m;
    m.set_kind(chat::CHANNEL_KIND_SCENE);
    m.set_phase(chat::PHASE_ACTIVE);
    m.set_membership_generation(3);
    *m.mutable_epoch() = epoch();
    m.set_join_watermark(10);
    m.set_last_client_send_seq(1);
    *m.mutable_last_receipt() = receipt();
    return m;
}
SendChat send() {
    SendChat m;
    m.set_kind(chat::CHANNEL_KIND_SCENE);
    m.set_membership_generation(3);
    *m.mutable_expected_epoch() = epoch();
    m.set_client_send_seq(1);
    m.set_text("你好 👋");
    return m;
}
ChatSendResult result() {
    ChatSendResult m;
    m.set_kind(chat::CHANNEL_KIND_SCENE);
    m.set_membership_generation(3);
    *m.mutable_epoch() = epoch();
    m.set_client_send_seq(1);
    m.set_outcome(chat::OUTCOME_ACCEPTED);
    *m.mutable_receipt() = receipt();
    return m;
}
ChatMessage notification() {
    ChatMessage m;
    m.set_membership_generation(3);
    *m.mutable_epoch() = epoch();
    m.set_channel_seq(11);
    m.set_sender_character_id(7);
    m.set_text("hello");
    return m;
}
ChatInternalRequest request(chat::Operation operation) {
    ChatInternalRequest m;
    *m.mutable_link() = link(operation == chat::OPERATION_HELLO);
    m.set_operation(operation);
    if (operation != chat::OPERATION_HELLO) *m.mutable_membership() = membership();
    if (operation == chat::OPERATION_SEND) *m.mutable_send() = send();
    return m;
}
ChatInternalReply reply(chat::Operation operation) {
    ChatInternalReply m;
    *m.mutable_link() = link();
    m.set_operation(operation);
    m.set_status(chat::STATUS_OK);
    if (operation == chat::OPERATION_HELLO) return m;
    *m.mutable_membership() = membership();
    if (operation == chat::OPERATION_JOIN || operation == chat::OPERATION_STATE)
        *m.mutable_state() = state();
    if (operation == chat::OPERATION_SEND) *m.mutable_result() = result();
    return m;
}
ChatInternalPush push() {
    ChatInternalPush m;
    *m.mutable_link() = link();
    *m.mutable_membership() = membership();
    *m.mutable_message() = notification();
    return m;
}
void roundtrip(const auto& message, auto decode, std::uint32_t type, std::uint64_t id) {
    const auto bytes = encode(message, id);
    const auto decoded = decode(bytes);
    ASSERT_TRUE(decoded);
    EXPECT_EQ(decoded->SerializeAsString(), message.SerializeAsString());
    Envelope envelope;
    ASSERT_TRUE(envelope.ParseFromArray(bytes.data(), static_cast<int>(bytes.size())));
    EXPECT_EQ(envelope.protocol_version(), 1U);
    EXPECT_EQ(envelope.message_id(), type);
    EXPECT_EQ(envelope.request_id(), id);
}

TEST(ChatProtocolTest, AllClientMessagesUseEnvelopeV1AndExpectedCorrelation) {
    GetChatState get;
    get.set_kind(chat::CHANNEL_KIND_SCENE);
    roundtrip(get, decode_get_chat_state, 1701, 42);
    roundtrip(state(), decode_chat_state, 1702, 42);
    roundtrip(send(), decode_send_chat, 1703, 42);
    roundtrip(result(), decode_chat_send_result, 1704, 42);
    roundtrip(notification(), decode_chat_message, 1705, 0);
    ChatUnavailable unavailable;
    unavailable.set_kind(chat::CHANNEL_KIND_REALM);
    unavailable.set_reason(chat::REASON_GAP);
    unavailable.set_rejoin_required(true);
    roundtrip(unavailable, decode_chat_unavailable, 1706, 0);
    EXPECT_EQ(chat_request_id(encode(get, 42)), 42U);
    EXPECT_EQ(chat_request_id(encode(notification())), 0U);
}

TEST(ChatProtocolTest, InternalHelloJoinLeaveSendStateAndPushRoundTrip) {
    for (int value = 1; value <= 5; ++value) {
        const auto operation = static_cast<chat::Operation>(value);
        roundtrip(request(operation), decode_chat_internal_request, 1801, 42);
        roundtrip(reply(operation), decode_chat_internal_reply, 1802, 42);
        for (int code = 2; code <= 7; ++code) {
            auto failed = reply(operation);
            failed.set_status(static_cast<chat::Status>(code));
            failed.clear_membership(); failed.clear_state(); failed.clear_result();
            if (operation == chat::OPERATION_HELLO) *failed.mutable_link() = link(true);
            roundtrip(failed, decode_chat_internal_reply, 1802, 42);
        }
    }
    roundtrip(push(), decode_chat_internal_push, 1803, 0);
    EXPECT_EQ(chat_internal_request_id(encode(request(chat::OPERATION_HELLO), 42)), 42U);
    EXPECT_EQ(chat_internal_request_id(encode(push())), 0U);
}

TEST(ChatProtocolTest, ClientInternalAndEarlierMessageGroupsCannotClaimEachOther) {
    const auto external = encode(send(), 42);
    const auto internal = encode(request(chat::OPERATION_SEND), 42);
    EXPECT_FALSE(chat_message_id(internal));
    EXPECT_FALSE(chat_internal_message_id(external));
    EXPECT_FALSE(decode_send_chat(internal));
    EXPECT_FALSE(decode_chat_internal_request(external));
    for (const auto& bytes : {external, internal}) {
        EXPECT_FALSE(edge_message_id(bytes)); EXPECT_FALSE(realm_message_id(bytes));
        EXPECT_FALSE(scene_message_id(bytes)); EXPECT_FALSE(scene_internal_message_id(bytes));
    }
    EXPECT_FALSE(chat_message_id(encode(HeartbeatRequest{}, 1)));
    EXPECT_FALSE(chat_internal_message_id(encode(ListCharacters{}, 1)));
    EXPECT_FALSE(chat_message_id(encode(GetSceneState{}, 1)));
}

TEST(ChatProtocolTest, TextAcceptsExactBytesWithoutNormalization) {
    for (const std::string text : std::initializer_list<std::string>{" ", "hello", "你好", "😀", "é", "é", std::string(512, 'x')}) {
        auto m = send(); m.set_text(text);
        ASSERT_TRUE(valid_chat_text(text));
        const auto decoded = decode_send_chat(encode(m, 1));
        ASSERT_TRUE(decoded); EXPECT_EQ(decoded->text(), text);
    }
    auto m = send();
    m.set_text(std::string(128, 'x') + std::string(384, 'y'));
    EXPECT_TRUE(decode_send_chat(encode(m, 1)));
    m.set_text(std::string(513, 'x'));
    EXPECT_THROW(static_cast<void>(encode(m, 1)), std::invalid_argument);
    EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
}

TEST(ChatProtocolTest, MalformedUnicodeNulAndAllAsciiControlsAreRejectedOnBothLines) {
    for (unsigned int control = 0; control <= 127; ++control) {
        if (control >= 32 && control != 127) continue;
        auto m = send(); m.set_text(std::string("a") + static_cast<char>(control) + "b");
        EXPECT_FALSE(valid_chat_text(m.text()));
        EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
    }
    const std::string bad[] = {"", "\x80", "\xc0\xaf", "\xc1\xbf", "\xc2", "\xe0\x80\xaf",
        "\xed\xa0\x80", "\xed\xbf\xbf", "\xf0\x80\x80\xaf", "\xf4\x90\x80\x80",
        "\xf5\x80\x80\x80", "\xff", "\xe4\xb8", "\xc2x"};
    for (const auto& text : bad) {
        SCOPED_TRACE(text.size());
        auto m = send(); m.set_text(text);
        EXPECT_FALSE(valid_chat_text(text)); EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
        EXPECT_THROW(static_cast<void>(encode(m, 1)), std::invalid_argument);
        auto internal = request(chat::OPERATION_SEND); internal.mutable_send()->set_text(text);
        EXPECT_FALSE(decode_chat_internal_request(raw(internal, 1801)));
        auto event = notification(); event.set_text(text);
        EXPECT_FALSE(decode_chat_message(raw(event, 1705, 0)));
    }
    EXPECT_TRUE(valid_chat_text("\xc2\x80\xdf\xbf\xe0\xa0\x80\xed\x9f\xbf\xee\x80\x80\xef\xbf\xbf\xf0\x90\x80\x80\xf4\x8f\xbf\xbf"));
}

TEST(ChatProtocolTest, RequiredFieldsCountersEnumsAndDirectionFailClosed) {
    EXPECT_FALSE(decode_send_chat(raw(SendChat{}, 1703)));
    EXPECT_FALSE(decode_get_chat_state(raw(GetChatState{}, 1701)));
    for (const auto seq : {std::uint64_t{0}, chat_counter_max + 1, std::numeric_limits<std::uint64_t>::max()}) {
        auto m = send(); m.set_client_send_seq(seq);
        EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
        m = send(); m.set_membership_generation(seq);
        EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
    }
    auto m = send(); m.set_client_send_seq(chat_counter_max); m.set_membership_generation(chat_counter_max);
    EXPECT_TRUE(decode_send_chat(encode(m, 1)));
    m.set_kind(static_cast<chat::ChannelKind>(99)); EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
    EXPECT_FALSE(decode_send_chat(raw(send(), 1703, 0)));
    EXPECT_FALSE(decode_chat_state(raw(state(), 1702, 0)));
    EXPECT_FALSE(decode_chat_message(raw(notification(), 1705, 1)));
    EXPECT_FALSE(decode_chat_internal_request(raw(request(chat::OPERATION_JOIN), 1801, 0)));
    EXPECT_FALSE(decode_chat_internal_push(raw(push(), 1803, 1)));
}

TEST(ChatProtocolTest, ChannelAndMembershipShapesDoNotGrantForgedRealmOrShardAuthority) {
    auto m = send(); m.mutable_expected_epoch()->mutable_channel()->set_shard_id(3);
    EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
    m = send(); m.set_kind(chat::CHANNEL_KIND_REALM);
    EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
    *m.mutable_expected_epoch() = epoch(chat::CHANNEL_KIND_REALM);
    EXPECT_TRUE(decode_send_chat(encode(m, 1)));
    m.mutable_expected_epoch()->mutable_channel()->set_shard_id(1);
    EXPECT_FALSE(decode_send_chat(raw(m, 1703)));
    auto r = request(chat::OPERATION_SEND);
    r.mutable_membership()->mutable_channel()->set_realm_id(2);
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    r = request(chat::OPERATION_SEND); r.mutable_membership()->set_character_id(0);
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    r = request(chat::OPERATION_SEND); r.mutable_membership()->set_generation(4);
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    r = request(chat::OPERATION_JOIN); r.mutable_membership()->set_scene_binding_epoch(0);
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    *r.mutable_membership() = membership(chat::CHANNEL_KIND_REALM);
    EXPECT_TRUE(decode_chat_internal_request(encode(r, 1)));
    r.mutable_membership()->set_scene_binding_epoch(7);
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    // 非零的伪造账号/角色无法由无状态编解码识别；C2/C3 必须核对认证与当前会话。
}

TEST(ChatProtocolTest, ActiveStatePreservesOriginalJoinWatermarkAndReceiptContext) {
    auto m = state();
    EXPECT_TRUE(decode_chat_state(encode(m, 1)));
    m.set_join_watermark(11); EXPECT_FALSE(decode_chat_state(raw(m, 1702)));
    m = state(); m.set_last_client_send_seq(2); EXPECT_FALSE(decode_chat_state(raw(m, 1702)));
    m = state(); m.mutable_last_receipt()->mutable_epoch()->set_chat_incarnation("old");
    EXPECT_FALSE(decode_chat_state(raw(m, 1702)));
    m = state(); m.clear_last_receipt(); EXPECT_FALSE(decode_chat_state(raw(m, 1702)));
    m.set_last_client_send_seq(0); m.set_join_watermark(0);
    EXPECT_TRUE(decode_chat_state(encode(m, 1)));
    m = state(); m.set_phase(chat::PHASE_UNAVAILABLE); m.set_reason(chat::REASON_NOT_MEMBER);
    EXPECT_FALSE(decode_chat_state(raw(m, 1702)));
    m.clear_epoch(); m.clear_last_receipt(); m.set_membership_generation(0);
    m.set_join_watermark(0); m.set_last_client_send_seq(0);
    roundtrip(m, decode_chat_state, 1702, 1);
    m.set_reason(chat::REASON_NONE); EXPECT_FALSE(decode_chat_state(raw(m, 1702)));
}

TEST(ChatProtocolTest, AcceptedRejectedAndUnknownHaveDisjointReceiptShapes) {
    auto m = result();
    m.clear_receipt(); EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
    m = result(); m.mutable_receipt()->set_client_send_seq(2);
    EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
    m = result(); m.mutable_receipt()->mutable_epoch()->mutable_channel()->set_shard_id(2);
    EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
    for (const auto outcome : {chat::OUTCOME_REJECTED, chat::OUTCOME_UNKNOWN}) {
        m = result(); m.set_outcome(outcome); m.set_reason(chat::REASON_DEADLINE);
        EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
        m.clear_receipt(); roundtrip(m, decode_chat_send_result, 1704, 1);
    }
    m.set_reason(chat::REASON_INVALID_TEXT); EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
    m.set_reason(chat::REASON_OUTCOME_LOST); roundtrip(m, decode_chat_send_result, 1704, 1);
    m.set_outcome(chat::OUTCOME_REJECTED); EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
    m.set_reason(chat::REASON_RATE_LIMITED); EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
    m.set_retry_after_ms(1000); roundtrip(m, decode_chat_send_result, 1704, 1);
    m.set_retry_after_ms(1001); EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
    m = result(); m.set_outcome(static_cast<chat::Outcome>(99)); EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
    m = result(); m.set_reason(static_cast<chat::Reason>(99)); EXPECT_FALSE(decode_chat_send_result(raw(m, 1704)));
}

TEST(ChatProtocolTest, InternalOperationFieldsAndLinkIncarnationsMustMatch) {
    auto r = request(chat::OPERATION_HELLO);
    *r.mutable_membership() = membership(); EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    r = request(chat::OPERATION_JOIN); *r.mutable_send() = send();
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    r = request(chat::OPERATION_SEND); r.mutable_link()->set_chat_incarnation("old");
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    r = request(chat::OPERATION_SEND); r.mutable_membership()->set_realm_incarnation("old");
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    r = request(chat::OPERATION_HELLO); r.mutable_link()->set_link_generation(1);
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    r = request(chat::OPERATION_JOIN); r.mutable_link()->set_link_generation(0);
    EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    for (const std::string id : std::initializer_list<std::string>{"", "bad identity", std::string(129, 'x'), "换代"}) {
        r = request(chat::OPERATION_JOIN); r.mutable_link()->set_realm_instance_id(id);
        EXPECT_FALSE(decode_chat_internal_request(raw(r, 1801)));
    }
    auto p = push(); p.mutable_message()->set_membership_generation(2);
    EXPECT_FALSE(decode_chat_internal_push(raw(p, 1803, 0)));
    p = push(); p.mutable_message()->mutable_epoch()->set_chat_incarnation("old");
    EXPECT_FALSE(decode_chat_internal_push(raw(p, 1803, 0)));
    auto response = reply(chat::OPERATION_JOIN); response.mutable_state()->set_membership_generation(4);
    EXPECT_FALSE(decode_chat_internal_reply(raw(response, 1802)));
    response = reply(chat::OPERATION_JOIN); response.set_status(chat::STATUS_BUSY);
    EXPECT_FALSE(decode_chat_internal_reply(raw(response, 1802)));
    response = reply(chat::OPERATION_SEND); response.clear_result();
    EXPECT_FALSE(decode_chat_internal_reply(raw(response, 1802)));
    response = reply(chat::OPERATION_LEAVE); *response.mutable_state() = state();
    EXPECT_FALSE(decode_chat_internal_reply(raw(response, 1802)));
}

void append_varint(std::string& bytes, std::size_t value) {
    while (value >= 128) { bytes.push_back(static_cast<char>((value & 127U) | 128U)); value >>= 7U; }
    bytes.push_back(static_cast<char>(value));
}
std::string with_extension(const auto& message, std::size_t count) {
    auto bytes = message.SerializeAsString();
    append_varint(bytes, (99U << 3U) | 2U);
    append_varint(bytes, count); bytes.append(count, 'x');
    return bytes;
}
TEST(ChatProtocolTest, RejectsMalformedVersionWrongIdAndEnvelopeAbove64KiBButAllowsUnknownFields) {
    EXPECT_FALSE(decode_send_chat(raw(send(), 1703, 1, 2)));
    EXPECT_FALSE(decode_send_chat(raw(send(), 1702)));
    EXPECT_FALSE(decode_send_chat(protobuf_bytes("\xff")));
    Envelope envelope;
    envelope.set_protocol_version(1); envelope.set_message_id(1703); envelope.set_request_id(1);
    envelope.set_payload("\xff");
    EXPECT_FALSE(decode_send_chat(protobuf_bytes(envelope.SerializeAsString())));
    bool exact = false, above = false;
    for (std::size_t n = chat_envelope_max - 256; n <= chat_envelope_max; ++n) {
        envelope.set_payload(with_extension(send(), n));
        const auto wire = envelope.SerializeAsString();
        if (wire.size() == chat_envelope_max) {
            exact = true; EXPECT_TRUE(decode_send_chat(protobuf_bytes(wire)));
        }
        if (wire.size() == chat_envelope_max + 1) {
            above = true; EXPECT_FALSE(decode_send_chat(protobuf_bytes(wire)));
            EXPECT_FALSE(chat_message_id(protobuf_bytes(wire)));
            SendChat extended; ASSERT_TRUE(extended.ParseFromString(envelope.payload()));
            EXPECT_THROW(static_cast<void>(encode(extended, 1)), std::invalid_argument);
        }
    }
    EXPECT_TRUE(exact); EXPECT_TRUE(above);
    const std::vector<std::byte> oversized(chat_envelope_max + 1);
    EXPECT_FALSE(chat_internal_message_id(oversized));
    EXPECT_FALSE(decode_chat_internal_push(oversized));
    SendChat extended; ASSERT_TRUE(extended.ParseFromString(with_extension(send(), 10)));
    EXPECT_TRUE(decode_send_chat(encode(extended, 1)));
}

TEST(ChatProtocolTest, CountAndByteBudgetsRejectEitherLimitAndIntegerOverflow) {
    const ChatBudget limit{chat_client_notifications_max, chat_client_notification_bytes_max};
    EXPECT_TRUE(chat_budget_allows({255, 256 * 1024 - 512}, {1, 512}, limit));
    EXPECT_FALSE(chat_budget_allows({256, 0}, {1, 1}, limit));
    EXPECT_FALSE(chat_budget_allows({0, 256 * 1024}, {1, 1}, limit));
    EXPECT_FALSE(chat_budget_allows({257, 0}, {0, 0}, limit));
    EXPECT_FALSE(chat_budget_allows({0, 256 * 1024 + 1}, {0, 0}, limit));
    const auto max = std::numeric_limits<std::size_t>::max();
    EXPECT_FALSE(chat_budget_allows({max, max}, {1, 1}, {max, max}));
    EXPECT_TRUE(chat_budget_allows({max - 1, max - 1}, {1, 1}, {max, max}));
    EXPECT_FALSE(chat_budget_allows({1, 1}, {max, max}, {max, max}));
    const ChatBudget pushes{chat_pending_pushes_max, chat_pending_push_bytes_max};
    EXPECT_TRUE(chat_budget_allows({1023, 4 * 1024 * 1024 - 512}, {1, 512}, pushes));
    EXPECT_FALSE(chat_budget_allows({1024, 0}, {1, 512}, pushes));
    EXPECT_FALSE(chat_budget_allows({0, 4 * 1024 * 1024}, {1, 512}, pushes));
}
}  // namespace
}  // namespace realm::game::common
