#include "realmmesh/observability/metrics_registry.hpp"
#include "realmmesh/observability/scoped_timer.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <limits>
#include <stdexcept>
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

TEST(MetricsRegistryTest, CustomBucketsRenderInRegisteredOrder) {
    MetricsRegistry registry;
    registry.histogram_buckets(
        "frame_work_duration_seconds", {0.0001, 0.001, 0.01});
    registry.histogram_observe("frame_work_duration_seconds", 0.0005);
    registry.histogram_observe("frame_work_duration_seconds", 0.002);
    registry.histogram_observe("frame_work_duration_seconds", 0.5);

    const auto expected =
        "# TYPE frame_work_duration_seconds histogram\n"
        "frame_work_duration_seconds_bucket{le=\"0.0001\"} 0\n"
        "frame_work_duration_seconds_bucket{le=\"0.001\"} 1\n"
        "frame_work_duration_seconds_bucket{le=\"0.01\"} 2\n"
        "frame_work_duration_seconds_bucket{le=\"+Inf\"} 3\n"
        "frame_work_duration_seconds_sum 0.5025\n"
        "frame_work_duration_seconds_count 3\n";
    EXPECT_EQ(registry.render(), expected);
    // 渲染确定:重复抓取逐字节相同。
    EXPECT_EQ(registry.render(), expected);
}

TEST(MetricsRegistryTest, CustomBucketsApplyToEveryLabelSeriesOfTheName) {
    MetricsRegistry registry;
    registry.histogram_buckets("tick_seconds", {0.001});
    registry.histogram_observe("tick_seconds", 0.0005, {{"stage", "a"}});
    registry.histogram_observe("other_seconds", 0.0005);

    const auto text = registry.render();
    EXPECT_TRUE(
        contains_line(text, "tick_seconds_bucket{stage=\"a\",le=\"0.001\"} 1"));
    EXPECT_EQ(text.find("tick_seconds_bucket{stage=\"a\",le=\"0.005\"}"),
              std::string::npos);
    // 未注册的名字仍用默认桶。
    EXPECT_TRUE(contains_line(text, "other_seconds_bucket{le=\"0.005\"} 1"));
}

TEST(MetricsRegistryTest, CustomBucketsRejectInvalidOrLateRegistration) {
    MetricsRegistry registry;
    EXPECT_THROW(registry.histogram_buckets("a", {}), std::invalid_argument);
    EXPECT_THROW(
        registry.histogram_buckets("a", {0.1, 0.1}), std::invalid_argument);
    EXPECT_THROW(
        registry.histogram_buckets(
            "a", {0.1, std::numeric_limits<double>::infinity()}),
        std::invalid_argument);

    registry.histogram_buckets("b", {0.1, 0.2});
    registry.histogram_buckets("b", {0.1, 0.2});  // 相同布局重复注册无害。
    EXPECT_THROW(registry.histogram_buckets("b", {0.1}), std::logic_error);

    registry.histogram_observe("c", 1.0, {{"k", "v"}});
    EXPECT_THROW(registry.histogram_buckets("c", {0.1}), std::logic_error);
}

TEST(ScopedTimerTest, ObservesElapsedSecondsOnDestruction) {
    using namespace std::chrono_literals;
    MetricsRegistry registry;
    registry.histogram_buckets("work_seconds", {0.001, 10.0});
    {
        const ScopedTimer timer(registry, "work_seconds");
        std::this_thread::sleep_for(2ms);
        EXPECT_GE(timer.elapsed(), 2ms);
        // 析构前不记账。
        EXPECT_EQ(registry.render(), "");
    }

    const auto text = registry.render();
    EXPECT_TRUE(contains_line(text, "work_seconds_bucket{le=\"0.001\"} 0"));
    EXPECT_TRUE(contains_line(text, "work_seconds_bucket{le=\"10\"} 1"));
    EXPECT_TRUE(contains_line(text, "work_seconds_count 1"));
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
