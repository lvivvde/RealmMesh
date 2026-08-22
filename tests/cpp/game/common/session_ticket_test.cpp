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

TEST(SessionTicketTest, ValidatesPurposeExpiryTamperingAndReplay) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(1'000s);
    SessionTicketCodec codec(test_key());
    auto ticket = codec.issue(TicketPurpose::EnterGame, 7, 3, 99, 30s, now);

    const auto claims = codec.validate(ticket, TicketPurpose::EnterGame, now + 1s);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->account_id, 7U);
    EXPECT_EQ(claims->realm_id, 3U);
    EXPECT_EQ(claims->character_id, 99U);
    EXPECT_FALSE(codec.validate(ticket, TicketPurpose::Login, now).has_value());
    EXPECT_FALSE(codec.validate(ticket, TicketPurpose::EnterGame, now + 31s).has_value());

    TicketReplayGuard replay;
    EXPECT_TRUE(replay.consume(*claims, now));
    EXPECT_FALSE(replay.consume(*claims, now));
    ticket[10] ^= std::byte{1};
    EXPECT_FALSE(codec.validate(ticket, TicketPurpose::EnterGame, now).has_value());
}

TEST(SessionTicketTest, V2CarriesASignedCorrelationIdAndStillAcceptsV1) {
    using namespace std::chrono_literals;
    const auto now = std::chrono::system_clock::time_point(2'000s);
    SessionTicketCodec codec(test_key());
    CorrelationId correlation_id{};
    correlation_id.front() = std::byte{0x12};
    correlation_id.back() = std::byte{0x34};

    const auto v1 = codec.issue(TicketPurpose::Login, 7, 3, 0, 30s, now);
    auto v2 = codec.issue(
        TicketPurpose::Login,
        7,
        3,
        0,
        correlation_id,
        30s,
        now);

    const auto v1_claims = codec.validate(v1, TicketPurpose::Login, now + 1s);
    const auto v2_claims = codec.validate(v2, TicketPurpose::Login, now + 1s);
    ASSERT_TRUE(v1_claims.has_value());
    ASSERT_TRUE(v2_claims.has_value());
    EXPECT_FALSE(v1_claims->correlation_id.has_value());
    ASSERT_TRUE(v2_claims->correlation_id.has_value());
    EXPECT_EQ(*v2_claims->correlation_id, correlation_id);
    EXPECT_EQ(v1.front(), std::byte{1});
    EXPECT_EQ(v2.front(), std::byte{2});

    v2[2 + session_ticket_id_size] ^= std::byte{1};
    EXPECT_FALSE(codec.validate(v2, TicketPurpose::Login, now).has_value());
}

TEST(EdgeProtocolTest, RoundTripsTheThreeStageHandshakeMessages) {
    LoginRequest login;
    login.set_account("alice");
    login.set_credential("dev");
    const auto encoded_login = encode(login, 42);
    const auto decoded_login = decode_login_request(encoded_login);
    ASSERT_TRUE(decoded_login.has_value());
    EXPECT_EQ(decoded_login->account(), "alice");
    EXPECT_EQ(decoded_login->credential(), "dev");
    EXPECT_EQ(edge_request_id(encoded_login), 42);
    EXPECT_EQ(
        edge_message_id(encoded_login),
        ::realmmesh::protocol::edge::v1::MESSAGE_ID_C2S_LOGIN_REQUEST);
    EXPECT_FALSE(decode_enter_game(encoded_login).has_value());

    LoginSucceeded success;
    success.set_account_id(7);
    success.set_login_ticket("\x01\x02", 2);
    auto* realm_endpoint = success.add_realm_endpoints();
    realm_endpoint->set_protocol(edge_v1::TRANSPORT_PROTOCOL_TLS_TCP);
    realm_endpoint->set_address("realm.example.com");
    realm_endpoint->set_port(7100);
    realm_endpoint->set_priority(0);
    const auto decoded_success = decode_login_succeeded(encode(success));
    ASSERT_TRUE(decoded_success.has_value());
    EXPECT_EQ(decoded_success->account_id(), 7);
    EXPECT_EQ(decoded_success->login_ticket(), std::string("\x01\x02", 2));
    ASSERT_EQ(decoded_success->realm_endpoints_size(), 1);
    EXPECT_EQ(decoded_success->realm_endpoints(0).protocol(),
              edge_v1::TRANSPORT_PROTOCOL_TLS_TCP);
    EXPECT_EQ(decoded_success->realm_endpoints(0).address(), "realm.example.com");
    EXPECT_EQ(decoded_success->realm_endpoints(0).port(), 7100);

    CharacterList characters;
    auto* knight = characters.add_characters();
    knight->set_id(99);
    knight->set_name("Knight");
    auto* mage = characters.add_characters();
    mage->set_id(100);
    mage->set_name("Mage");
    const auto decoded_characters = decode_character_list(encode(characters));
    ASSERT_TRUE(decoded_characters.has_value());
    ASSERT_EQ(decoded_characters->characters_size(), 2);
    EXPECT_EQ(decoded_characters->characters(0).name(), "Knight");
    EXPECT_EQ(decoded_characters->characters(1).id(), 100);

    EnterGameIssued issued;
    issued.set_enter_game_ticket("\x03", 1);
    auto* quic_endpoint = issued.add_gateway_endpoints();
    quic_endpoint->set_protocol(edge_v1::TRANSPORT_PROTOCOL_QUIC);
    quic_endpoint->set_address("gateway.example.com");
    quic_endpoint->set_port(8000);
    quic_endpoint->set_priority(0);
    auto* tcp_endpoint = issued.add_gateway_endpoints();
    tcp_endpoint->set_protocol(edge_v1::TRANSPORT_PROTOCOL_TLS_TCP);
    tcp_endpoint->set_address("gateway.example.com");
    tcp_endpoint->set_port(8000);
    tcp_endpoint->set_priority(1);
    const auto decoded_issued = decode_enter_game_issued(encode(issued));
    ASSERT_TRUE(decoded_issued.has_value());
    EXPECT_EQ(decoded_issued->enter_game_ticket(), std::string("\x03", 1));
    ASSERT_EQ(decoded_issued->gateway_endpoints_size(), 2);
    EXPECT_EQ(decoded_issued->gateway_endpoints(0).protocol(),
              edge_v1::TRANSPORT_PROTOCOL_QUIC);
    EXPECT_EQ(decoded_issued->gateway_endpoints(1).protocol(),
              edge_v1::TRANSPORT_PROTOCOL_TLS_TCP);

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
