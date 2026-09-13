#include "realmmesh/loadgen/metrics_scrape.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace realm::loadgen {
namespace {

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
