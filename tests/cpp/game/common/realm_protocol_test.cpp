#include "realmmesh/game/common/realm_protocol.hpp"

#include "realmmesh/game/common/edge_protocol.hpp"

#include <gtest/gtest.h>

namespace realm::game::common {
namespace {

TEST(RealmProtocolTest, ListCharactersCarriesIdAndRequestId) {
    const auto wire = encode(ListCharacters{}, 5);
    EXPECT_EQ(
        realm_message_id(wire), RealmMessageId::MESSAGE_ID_C2S_LIST_CHARACTERS);
    EXPECT_EQ(realm_request_id(wire), 5U);
    EXPECT_TRUE(decode_list_characters(wire).has_value());
}

TEST(RealmProtocolTest, CharacterListRoundTripsItemsAndLastSelected) {
    CharacterList list;
    auto* item = list.add_characters();
    item->set_character_id(7);
    item->set_name("勇者");
    item->set_exp(30);
    item->set_level(1);
    list.set_last_selected_character_id(7);
    const auto decoded = decode_character_list(encode(list, 2));
    ASSERT_TRUE(decoded.has_value());
    ASSERT_EQ(decoded->characters_size(), 1);
    EXPECT_EQ(decoded->characters(0).name(), "勇者");
    EXPECT_EQ(decoded->characters(0).exp(), 30U);
    EXPECT_EQ(decoded->last_selected_character_id(), 7U);
}

TEST(RealmProtocolTest, TrainResultRoundTripsReplayedFlag) {
    TrainResult result;
    result.set_character_id(7);
    result.set_exp(40);
    result.set_level(1);
    result.set_seq(4);
    result.set_replayed(true);
    const auto wire = encode(result, 9);
    EXPECT_EQ(
        realm_message_id(wire), RealmMessageId::MESSAGE_ID_S2C_TRAIN_RESULT);
    const auto decoded = decode_train_result(wire);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->seq(), 4U);
    EXPECT_TRUE(decoded->replayed());
}

TEST(RealmProtocolTest, DisplacedIsAServerPush) {
    const auto wire = encode(RealmSessionDisplaced{});
    EXPECT_EQ(
        realm_message_id(wire),
        RealmMessageId::MESSAGE_ID_S2C_REALM_SESSION_DISPLACED);
    EXPECT_EQ(realm_request_id(wire), 0U);
    EXPECT_TRUE(decode_realm_session_displaced(wire).has_value());
}

/// Realm 与 Edge 两组编号互不认领:各自的 message_id 解析只接受本组。
TEST(RealmProtocolTest, EdgeAndRealmIdsDoNotOverlap) {
    const auto realm_wire = encode(Train{}, 1);
    EXPECT_FALSE(edge_message_id(realm_wire).has_value());
    EXPECT_FALSE(realm_message_id(encode(HeartbeatRequest{}, 1)).has_value());
}

TEST(RealmProtocolTest, DecodeRejectsWrongMessageId) {
    EXPECT_FALSE(decode_select_character(encode(Train{}, 1)).has_value());
}

TEST(RealmProtocolTest, ErrorCodesFollowSpec) {
    EXPECT_EQ(edge_error_phase_mismatch, 3003);
    EXPECT_EQ(edge_error_invalid_character_name, 3004);
    EXPECT_EQ(edge_error_character_name_taken, 3005);
    EXPECT_EQ(edge_error_character_limit_reached, 3006);
    EXPECT_EQ(edge_error_character_not_owned, 3007);
    EXPECT_EQ(edge_error_max_level, 3008);
    EXPECT_EQ(edge_error_invalid_training_seq, 3009);
    EXPECT_EQ(edge_error_realm_data_unavailable, 3010);
}

}  // namespace
}  // namespace realm::game::common
