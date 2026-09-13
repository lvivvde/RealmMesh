#include "realmmesh/observability/metrics_registry.hpp"

#include <gtest/gtest.h>

#include <string>
#include <thread>

namespace realm::observability {
namespace {

[[nodiscard]] bool contains_line(
    const std::string& text, std::string_view line) {
    return text.find(std::string(line) + '\n') != std::string::npos;
}

TEST(MetricsRegistryTest, CounterAddRendersTypeLineAndCumulativeValue) {
    MetricsRegistry registry;
    registry.counter_add("verify_requests_total");
    registry.counter_add("verify_requests_total", 2.0);

    const auto text = registry.render();
    EXPECT_TRUE(contains_line(text, "# TYPE verify_requests_total counter"));
    EXPECT_TRUE(contains_line(text, "verify_requests_total 3"));
}

TEST(MetricsRegistryTest, CounterSetPublishesPolledCumulativeValue) {
    MetricsRegistry registry;
    registry.counter_set("edge_fetch_retry_total", 7.0);
    registry.counter_set("edge_fetch_retry_total", 9.0);

    const auto text = registry.render();
    EXPECT_TRUE(contains_line(text, "edge_fetch_retry_total 9"));
}

TEST(MetricsRegistryTest, GaugeSetWithLabelsRendersLabelSeries) {
    MetricsRegistry registry;
    registry.gauge_set("edge_budget", 42.0, {{"kind", "conn_free"}});
    registry.gauge_set("edge_budget", 3.0, {{"kind", "fetch_free"}});

    const auto text = registry.render();
    EXPECT_TRUE(contains_line(text, "# TYPE edge_budget gauge"));
    EXPECT_TRUE(contains_line(text, "edge_budget{kind=\"conn_free\"} 42"));
    EXPECT_TRUE(contains_line(text, "edge_budget{kind=\"fetch_free\"} 3"));
}

TEST(MetricsRegistryTest, HistogramRendersBucketsSumAndCount) {
    MetricsRegistry registry;
    registry.histogram_observe("verify_issue_duration_seconds", 0.08);
    registry.histogram_observe("verify_issue_duration_seconds", 0.3);

    const auto text = registry.render();
    EXPECT_TRUE(contains_line(
        text, "# TYPE verify_issue_duration_seconds histogram"));
    EXPECT_TRUE(contains_line(
        text, "verify_issue_duration_seconds_bucket{le=\"0.1\"} 1"));
    EXPECT_TRUE(contains_line(
        text, "verify_issue_duration_seconds_bucket{le=\"0.5\"} 2"));
    EXPECT_TRUE(contains_line(
        text, "verify_issue_duration_seconds_bucket{le=\"+Inf\"} 2"));
    EXPECT_TRUE(contains_line(text, "verify_issue_duration_seconds_sum 0.38"));
    EXPECT_TRUE(contains_line(text, "verify_issue_duration_seconds_count 2"));
}

TEST(MetricsRegistryTest, RenderIsSortedByMetricNameThenLabelSeries) {
    MetricsRegistry registry;
    registry.gauge_set("released_number", 5.0);
    registry.gauge_set("admit_rate", 2.0);
    registry.counter_add("tickets_issued_total");

    const auto text = registry.render();
    const auto admit = text.find("admit_rate");
    const auto released = text.find("released_number");
    const auto tickets = text.find("tickets_issued_total");
    ASSERT_NE(admit, std::string::npos);
    ASSERT_NE(released, std::string::npos);
    ASSERT_NE(tickets, std::string::npos);
    EXPECT_LT(admit, released);
    EXPECT_LT(released, tickets);
}

TEST(MetricsRegistryTest, LabelValuesAreEscaped) {
    MetricsRegistry registry;
    registry.gauge_set(
        "edge_sessions", 1.0, {{"stage", "a\"b\\c"}});

    EXPECT_TRUE(contains_line(
        registry.render(), "edge_sessions{stage=\"a\\\"b\\\\c\"} 1"));
}

TEST(MetricsRegistryTest, ConcurrentUpdatesKeepCountsConsistent) {
    MetricsRegistry registry;
    static constexpr int threads = 4;
    static constexpr int adds = 1'000;

    std::vector<std::thread> workers;
    for (int i = 0; i < threads; ++i) {
        workers.emplace_back([&registry] {
            for (int j = 0; j < adds; ++j) {
                registry.counter_add("progress_requests_total");
            }
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    EXPECT_TRUE(contains_line(
        registry.render(),
        "progress_requests_total " + std::to_string(threads * adds)));
}

}  // namespace
}  // namespace realm::observability
