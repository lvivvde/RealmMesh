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

/// #45 handoff:服务器推送的直连凭证(request_id=0)。
TEST(EdgeAttachProtocolTest, EnterRealmGrantedRoundTripsTicketAndEndpoint) {
    EnterRealmGranted granted;
    granted.set_enter_realm_ticket(std::string("\x07", 1));
    auto* endpoint = granted.add_realm_endpoints();
    endpoint->set_address("127.0.0.1");
    endpoint->set_port(7100);
    endpoint->set_protocol(
        EdgeTransportProtocol::TRANSPORT_PROTOCOL_TLS_TCP);

    const auto wire = encode(granted);
    EXPECT_EQ(
        edge_message_id(wire),
        EdgeMessageId::MESSAGE_ID_S2C_ENTER_REALM_GRANTED);
    EXPECT_EQ(edge_request_id(wire), 0U);

    const auto decoded = decode_enter_realm_granted(wire);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->enter_realm_ticket(), std::string("\x07", 1));
    ASSERT_EQ(decoded->realm_endpoints_size(), 1);
    EXPECT_EQ(decoded->realm_endpoints(0).address(), "127.0.0.1");
    EXPECT_EQ(decoded->realm_endpoints(0).port(), 7100);
    EXPECT_EQ(
        decoded->realm_endpoints(0).protocol(),
        EdgeTransportProtocol::TRANSPORT_PROTOCOL_TLS_TCP);
}

}  // namespace
}  // namespace realm::game::common
