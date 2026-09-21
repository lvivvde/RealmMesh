#include "realmmesh/network/client/json_field.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace realm::network::client {
namespace {

TEST(ExtractJsonStringFieldTest, ReadsIdentityTokenFromVerifyResponse) {
    const std::string_view body =
        R"({"identity_token":"abc.def.ghi","account_id":"42","expires_in":600})";
    const auto token = extract_json_string_field(body, "identity_token");
    ASSERT_TRUE(token.has_value());
    EXPECT_EQ(*token, "abc.def.ghi");
    EXPECT_EQ(extract_json_string_field(body, "account_id"), "42");
    EXPECT_EQ(extract_json_string_field(body, "missing"), std::nullopt);
}

TEST(ExtractJsonStringFieldTest, ReadsAdmissionGrantFromNestedAdmitObject) {
    // 放行响应:admit_grant 嵌套拼接(服务端 JsonCodec 只编扁平对象)。
    const std::string_view body =
        R"({"admit_grant":{"admission_grant":"grant-token","number":7,)"
        R"("expires_in":60},"status":"admitted","position":0,)"
        R"("estimated_wait_seconds":0})";
    const auto grant = extract_json_string_field(body, "admission_grant");
    ASSERT_TRUE(grant.has_value());
    EXPECT_EQ(*grant, "grant-token");
    EXPECT_EQ(extract_json_string_field(body, "status"), "admitted");
}

TEST(ExtractJsonStringFieldTest, RejectsMalformedValues) {
    EXPECT_EQ(extract_json_string_field("no fields", "name"), std::nullopt);
    EXPECT_EQ(extract_json_string_field(R"({"name":"unterminated)", "name"),
              std::nullopt);
    EXPECT_EQ(extract_json_string_field(R"({"name":42})", "name"),
              std::nullopt);
}

TEST(ExtractJsonIntFieldTest, ReadsNumbersFromTicketsResponse) {
    const std::string_view body =
        R"({"queue_number_token":"tok","number":1234,"estimated_wait_seconds":9})";
    const auto number = extract_json_int_field(body, "number");
    ASSERT_TRUE(number.has_value());
    EXPECT_EQ(*number, 1234);
    EXPECT_EQ(extract_json_int_field(body, "estimated_wait_seconds"), 9);
    EXPECT_EQ(extract_json_int_field(body, "queue_number_token"),
              std::nullopt);
    EXPECT_EQ(extract_json_int_field("{}", "number"), std::nullopt);
}

}  // namespace
}  // namespace realm::network::client
