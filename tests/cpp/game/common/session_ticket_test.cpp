#include "realmmesh/game/common/edge_protocol.hpp"
#include "realmmesh/game/common/session_ticket.hpp"

#include <gtest/gtest.h>

#include <chrono>

namespace realm::game::common {
namespace {

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

TEST(EdgeProtocolTest, RoundTripsTheThreeStageHandshakeMessages) {
    LoginRequest login{"alice", "dev"};
    EXPECT_EQ(decode_login_request(encode(login)), login);

    LoginSucceeded success{7, {std::byte{1}, std::byte{2}}, {"127.0.0.1", 7100}};
    EXPECT_EQ(decode_login_succeeded(encode(success)), success);

    CharacterList characters{{{99, "Knight"}, {100, "Mage"}}};
    EXPECT_EQ(decode_character_list(encode(characters)), characters);

    EnterGameIssued issued{{std::byte{3}}, {"127.0.0.1", 8000}};
    EXPECT_EQ(decode_enter_game_issued(encode(issued)), issued);
}

}  // namespace
}  // namespace realm::game::common
