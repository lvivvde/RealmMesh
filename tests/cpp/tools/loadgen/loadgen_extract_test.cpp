#include "realmmesh/loadgen/json_field.hpp"
#include "realmmesh/loadgen/metrics_scrape.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace realm::loadgen {
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

TEST(ExtractJsonStringFieldTest, ReadsGrantFromNestedAdmitObject) {
    // 放行响应:admit_grant 嵌套拼接(服务端 JsonCodec 只编扁平对象),
    // grant 是响应体内首个 queue_number_token。
    const std::string_view body =
        R"({"admit_grant":{"queue_number_token":"grant-token","number":7,)"
        R"("expires_in":60},"status":"admitted","position":0,)"
        R"("estimated_wait_seconds":0})";
    const auto grant = extract_json_string_field(body, "queue_number_token");
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

TEST(ParseMetricsLineTest, ParsesPlainAndLabeledSeries) {
    MetricsSnapshot snapshot;
    parse_metrics_line("# HELP verify_requests_total help text", snapshot);
    parse_metrics_line("verify_requests_total 5", snapshot);
    parse_metrics_line("edge_sessions{stage=\"handed_off\"} 2", snapshot);
    parse_metrics_line("edge_sessions{stage=\"fetching\"} 1.5", snapshot);
    parse_metrics_line("malformed-no-value", snapshot);
    parse_metrics_line("nan_value not-a-number", snapshot);

    EXPECT_EQ(snapshot.total("verify_requests_total"), 5);
    EXPECT_EQ(snapshot.total("edge_sessions"), 3.5);
    EXPECT_TRUE(snapshot.has("edge_sessions"));
    EXPECT_FALSE(snapshot.has("missing_metric"));
    // 前缀同名不算命中(name 边界)。
    parse_metrics_line("verify_requests_total_extra 9", snapshot);
    EXPECT_EQ(snapshot.total("verify_requests_total"), 5);
}

TEST(ScrapeMetricsTest, ParsesPrometheusBody) {
    MetricsSnapshot snapshot;
    std::string_view body{
        "# TYPE realmmesh_service_ready gauge\n"
        "realmmesh_service_ready 1\n"
        "tickets_issued_total 100\n"
        "edge_sessions{stage=\"handed_off\"} 42\n"};
    while (!body.empty()) {
        const auto newline = body.find('\n');
        parse_metrics_line(body.substr(0, newline), snapshot);
        body = newline == std::string_view::npos
                   ? std::string_view{}
                   : body.substr(newline + 1);
    }
    EXPECT_EQ(snapshot.total("tickets_issued_total"), 100);
    EXPECT_EQ(snapshot.total("edge_sessions"), 42);
}

}  // namespace
}  // namespace realm::loadgen
