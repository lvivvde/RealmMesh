#include "realmmesh/game/common/compact_jws.hpp"
#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace realm::game::common {
namespace {

namespace edge_v1 = ::realmmesh::protocol::edge::v1;

SessionTicketKey test_key() {
    SessionTicketKey key{};
    key.front() = std::byte{1};
    return key;
}

/// 旧链退役的用途数值(Login=1 / EnterGame=2):枚举里已不存在,只能用
/// 整数构造。它们永久退役,不得被任何活着的调用点当作期望用途。
TicketPurpose retired_purpose(std::uint8_t value) {
    return static_cast<TicketPurpose>(value);
}

TEST(SessionTicketTest, ValidatesPurposeExpiryTamperingAndReplay) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(1'000s);
    SessionTicketCodec codec(test_key());
    auto ticket = codec.issue(TicketPurpose::EnterRealm, 7, 3, 99, 30s, now);

    const auto claims =
        codec.validate(ticket, TicketPurpose::EnterRealm, now + 1s);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->account_id, 7U);
    EXPECT_EQ(claims->realm_id, 3U);
    EXPECT_EQ(claims->character_id, 99U);
    EXPECT_FALSE(
        codec.validate(ticket, retired_purpose(1), now).has_value());
    EXPECT_FALSE(
        codec.validate(ticket,
                       TicketPurpose::EnterRealm,
                       now + 30s + jws_clock_leeway + 1s)
            .has_value());

    TicketReplayGuard replay;
    EXPECT_TRUE(replay.consume(*claims, now));
    EXPECT_FALSE(replay.consume(*claims, now));
    ticket[10] ^= std::byte{1};
    EXPECT_FALSE(
        codec.validate(ticket, TicketPurpose::EnterRealm, now).has_value());
}

TEST(SessionTicketTest, V2CarriesASignedCorrelationIdAndStillAcceptsV1) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(2'000s);
    SessionTicketCodec codec(test_key());
    CorrelationId correlation_id{};
    correlation_id.front() = std::byte{0x12};
    correlation_id.back() = std::byte{0x34};

    const auto v1 = codec.issue(TicketPurpose::EnterRealm, 7, 3, 0, 30s, now);
    auto v2 = codec.issue(
        TicketPurpose::EnterRealm, 7, 3, 0, correlation_id, 30s, now);

    const auto v1_claims =
        codec.validate(v1, TicketPurpose::EnterRealm, now + 1s);
    const auto v2_claims =
        codec.validate(v2, TicketPurpose::EnterRealm, now + 1s);
    ASSERT_TRUE(v1_claims.has_value());
    ASSERT_TRUE(v2_claims.has_value());
    EXPECT_FALSE(v1_claims->correlation_id.has_value());
    ASSERT_TRUE(v2_claims->correlation_id.has_value());
    EXPECT_EQ(*v2_claims->correlation_id, correlation_id);
    EXPECT_EQ(v1.front(), std::byte{1});
    EXPECT_EQ(v2.front(), std::byte{2});

    v2[2 + session_ticket_id_size] ^= std::byte{1};
    EXPECT_FALSE(
        codec.validate(v2, TicketPurpose::EnterRealm, now).has_value());
}

TEST(SessionTicketTest, RedeemsATicketExactlyOnce) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(3'000s);
    SessionTickets tickets(test_key());
    const auto ticket =
        tickets.issue(TicketPurpose::EnterRealm, 7, 3, 0, 30s, now);

    const auto first =
        tickets.redeem(ticket, TicketPurpose::EnterRealm, now + 1s);
    ASSERT_EQ(first.status, RedeemStatus::Accepted);
    EXPECT_EQ(first.claims.account_id, 7U);
    EXPECT_EQ(first.claims.realm_id, 3U);
    EXPECT_EQ(first.claims.purpose, TicketPurpose::EnterRealm);

    const auto second =
        tickets.redeem(ticket, TicketPurpose::EnterRealm, now + 2s);
    EXPECT_EQ(second.status, RedeemStatus::Replayed);
}

/// 用途字节在签名内:拿退役数值签发的票据,活着的兑换点不认;活用途的
/// 票据拿退役数值当期望用途也不认。删掉枚举取值不等于放弃用途校验。
TEST(SessionTicketTest, PurposeMismatchDoesNotBurnTheTicket) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(4'000s);
    SessionTickets tickets(test_key());
    const auto ticket =
        tickets.issue(TicketPurpose::EnterRealm, 7, 3, 0, 30s, now);

    const auto retired =
        tickets.redeem(ticket, retired_purpose(1), now + 1s);
    EXPECT_EQ(retired.status, RedeemStatus::InvalidTicket);

    const auto right_purpose =
        tickets.redeem(ticket, TicketPurpose::EnterRealm, now + 2s);
    EXPECT_EQ(right_purpose.status, RedeemStatus::Accepted);
}

TEST(SessionTicketTest, RetiredPurposeTicketsAreNotRedeemable) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(7'000s);
    SessionTickets tickets(test_key());
    for (const std::uint8_t retired : {1U, 2U}) {
        const auto ticket =
            tickets.issue(retired_purpose(retired), 7, 3, 0, 30s, now);
        const auto redeemed =
            tickets.redeem(ticket, TicketPurpose::EnterRealm, now + 1s);
        EXPECT_EQ(redeemed.status, RedeemStatus::InvalidTicket)
            << "retired purpose " << static_cast<int>(retired);
    }
}

/// 活用途的数值本身就是线上标识:换号会让老包被新语义误读。
TEST(SessionTicketTest, EnterRealmPurposeValueIsPinned) {
    EXPECT_EQ(static_cast<std::uint8_t>(TicketPurpose::EnterRealm), 3U);
}

TEST(SessionTicketTest, ExpiredTicketsAreInvalidNotReplayed) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(5'000s);
    SessionTickets tickets(test_key());
    const auto ticket =
        tickets.issue(TicketPurpose::EnterRealm, 7, 3, 9, 30s, now);

    const auto expired = tickets.redeem(
        ticket, TicketPurpose::EnterRealm, now + 30s + jws_clock_leeway + 1s);
    EXPECT_EQ(expired.status, RedeemStatus::InvalidTicket);
}

/// 跨机部署的服务间时钟偏差:过期后仍在容差内的票据照常受理,否则几秒
/// 漂移就会让玩家白排一次队。
TEST(SessionTicketTest, AcceptsTicketsWithinClockLeeway) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(8'000s);
    SessionTickets tickets(test_key());

    const auto just_expired = tickets.redeem(
        tickets.issue(TicketPurpose::EnterRealm, 7, 1, 0, 60s, now),
        TicketPurpose::EnterRealm,
        now + 60s + 1s);
    EXPECT_EQ(just_expired.status, RedeemStatus::Accepted);

    const auto at_leeway_edge = tickets.redeem(
        tickets.issue(TicketPurpose::EnterRealm, 7, 1, 0, 60s, now),
        TicketPurpose::EnterRealm,
        now + 60s + jws_clock_leeway);
    EXPECT_EQ(at_leeway_edge.status, RedeemStatus::Accepted);
}

/// 容差放大的是「可接受的过期余量」,不是「可重放的次数」:守卫持有期
/// 必须覆盖整个容差窗口,否则已消费的票据会在守卫释放后重新可兑换。
TEST(SessionTicketTest, LeewayDoesNotOpenAReplayWindow) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(9'000s);
    SessionTickets tickets(test_key());
    const auto ticket =
        tickets.issue(TicketPurpose::EnterRealm, 7, 1, 0, 60s, now);

    const auto first =
        tickets.redeem(ticket, TicketPurpose::EnterRealm, now + 1s);
    ASSERT_EQ(first.status, RedeemStatus::Accepted);

    const auto replayed_at_edge = tickets.redeem(
        ticket, TicketPurpose::EnterRealm, now + 60s + jws_clock_leeway);
    EXPECT_EQ(replayed_at_edge.status, RedeemStatus::Replayed);
}

TEST(SessionTicketsTest, EnterRealmPurposeRoundTripsAndRejectsWrongPurpose) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(6'000s);
    SessionTickets tickets(test_key());
    const auto ticket =
        tickets.issue(TicketPurpose::EnterRealm, 42, 1, 0, 60s, now);

    const auto stale = tickets.redeem(
        tickets.issue(TicketPurpose::EnterRealm, 43, 1, 0, 60s, now),
        TicketPurpose::EnterRealm,
        now + 60s + jws_clock_leeway + 1s);
    EXPECT_EQ(stale.status, RedeemStatus::InvalidTicket);

    const auto wrong =
        tickets.redeem(ticket, retired_purpose(2), now + 1s);
    EXPECT_EQ(wrong.status, RedeemStatus::InvalidTicket);

    const auto right =
        tickets.redeem(ticket, TicketPurpose::EnterRealm, now + 1s);
    ASSERT_EQ(right.status, RedeemStatus::Accepted);
    EXPECT_EQ(right.claims.purpose, TicketPurpose::EnterRealm);
    EXPECT_EQ(right.claims.account_id, 42U);
    EXPECT_EQ(right.claims.realm_id, 1U);
    EXPECT_EQ(right.claims.character_id, 0U);

    const auto replayed =
        tickets.redeem(ticket, TicketPurpose::EnterRealm, now + 2s);
    EXPECT_EQ(replayed.status, RedeemStatus::Replayed);
}

TEST(EdgeProtocolTest, RoundTripsTheGatewayAndRealmMessages) {
    EdgeAttach attach;
    attach.set_identity_token("identity.jws");
        attach.set_admission_grant("grant.jws");
    const auto encoded_attach = encode(attach, 42);
    const auto decoded_attach = decode_edge_attach(encoded_attach);
    ASSERT_TRUE(decoded_attach.has_value());
    EXPECT_EQ(decoded_attach->identity_token(), "identity.jws");
    EXPECT_EQ(decoded_attach->admission_grant(), "grant.jws");
    EXPECT_EQ(edge_request_id(encoded_attach), 42);
    EXPECT_EQ(edge_message_id(encoded_attach), edge_v1::MESSAGE_ID_C2S_EDGE_ATTACH);
    EXPECT_FALSE(decode_enter_realm(encoded_attach).has_value());

    EdgeAttachAccepted attach_accepted;
    attach_accepted.set_account_id(7);
    const auto decoded_attach_accepted =
        decode_edge_attach_accepted(encode(attach_accepted));
    ASSERT_TRUE(decoded_attach_accepted.has_value());
    EXPECT_EQ(decoded_attach_accepted->account_id(), 7);

    EnterRealmGranted granted;
    granted.set_enter_realm_ticket("\x01\x02", 2);
    auto* realm_endpoint = granted.add_realm_endpoints();
    realm_endpoint->set_protocol(edge_v1::TRANSPORT_PROTOCOL_TLS_TCP);
    realm_endpoint->set_address("realm.example.com");
    realm_endpoint->set_port(7100);
    realm_endpoint->set_priority(0);
    const auto decoded_granted = decode_enter_realm_granted(encode(granted));
    ASSERT_TRUE(decoded_granted.has_value());
    EXPECT_EQ(decoded_granted->enter_realm_ticket(), std::string("\x01\x02", 2));
    ASSERT_EQ(decoded_granted->realm_endpoints_size(), 1);
    EXPECT_EQ(
        decoded_granted->realm_endpoints(0).protocol(),
        edge_v1::TRANSPORT_PROTOCOL_TLS_TCP);
    EXPECT_EQ(decoded_granted->realm_endpoints(0).address(), "realm.example.com");
    EXPECT_EQ(decoded_granted->realm_endpoints(0).port(), 7100);

    EnterRealm enter_realm;
    enter_realm.set_enter_realm_ticket("\x03", 1);
    const auto decoded_enter_realm = decode_enter_realm(encode(enter_realm, 9));
    ASSERT_TRUE(decoded_enter_realm.has_value());
    EXPECT_EQ(decoded_enter_realm->enter_realm_ticket(), std::string("\x03", 1));
    EXPECT_EQ(edge_request_id(encode(enter_realm, 9)), 9);

    EnterRealmAccepted enter_realm_accepted;
    enter_realm_accepted.set_account_id(11);
    const auto decoded_accepted =
        decode_enter_realm_accepted(encode(enter_realm_accepted));
    ASSERT_TRUE(decoded_accepted.has_value());
    EXPECT_EQ(decoded_accepted->account_id(), 11);

    HeartbeatRequest heartbeat_request;
    const auto encoded_heartbeat_request = encode(heartbeat_request, 43);
    EXPECT_EQ(edge_request_id(encoded_heartbeat_request), 43);
    EXPECT_TRUE(
        decode_heartbeat_request(encoded_heartbeat_request).has_value());
    HeartbeatResponse heartbeat_response;
    EXPECT_TRUE(
        decode_heartbeat_response(encode(heartbeat_response, 43)).has_value());

    EdgeError error;
    error.set_code(1999);
    error.set_message("test error");
    const auto decoded_error = decode_edge_error(encode(error));
    ASSERT_TRUE(decoded_error.has_value());
    EXPECT_EQ(decoded_error->code(), 1999);
    EXPECT_EQ(decoded_error->message(), "test error");
}

}  // namespace
}  // namespace realm::game::common
