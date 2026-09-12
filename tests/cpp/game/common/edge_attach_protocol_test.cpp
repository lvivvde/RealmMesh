#include "realmmesh/game/common/edge_protocol.hpp"

#include <gtest/gtest.h>

namespace realm::game::common {
namespace {

TEST(EdgeAttachProtocolTest, AttachRoundTripsTokens) {
    EdgeAttach attach;
    attach.set_identity_token("identity-token");
    attach.set_queue_number_token("number-token");
    const auto wire = encode(attach, 7);
    EXPECT_EQ(
        edge_message_id(wire),
        EdgeMessageId::MESSAGE_ID_C2S_EDGE_ATTACH);
    EXPECT_EQ(edge_request_id(wire), 7U);
    const auto decoded = decode_edge_attach(wire);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->identity_token(), "identity-token");
    EXPECT_EQ(decoded->queue_number_token(), "number-token");
}

TEST(EdgeAttachProtocolTest, AttachAcceptedRoundTripsAccountId) {
    EdgeAttachAccepted accepted;
    accepted.set_account_id(42);
    const auto wire = encode(accepted, 9);
    EXPECT_EQ(
        edge_message_id(wire),
        EdgeMessageId::MESSAGE_ID_S2C_EDGE_ATTACH_ACCEPTED);
    EXPECT_EQ(edge_request_id(wire), 9U);
    const auto decoded = decode_edge_attach_accepted(wire);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->account_id(), 42U);
}

TEST(EdgeAttachProtocolTest, AttachErrorCodesFollowSpec) {
    EXPECT_EQ(edge_error_invalid_credentials, 1001);
    EXPECT_EQ(edge_error_attach_out_of_budget, 1004);
    EXPECT_EQ(edge_error_invalid_queue_number, 2001);
}

}  // namespace
}  // namespace realm::game::common
