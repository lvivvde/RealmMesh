#include "realmmesh/loadgen/loadgen.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace realm::loadgen {
namespace {

TEST(LatencyRecorderTest, EmptyRecorderReportsZeros) {
    LatencyRecorder recorder;
    EXPECT_EQ(recorder.samples(), 0U);
    EXPECT_EQ(recorder.percentile(0.5), 0);
    EXPECT_EQ(recorder.max(), 0);
}

TEST(LatencyRecorderTest, PercentilesFollowNearestRank) {
    LatencyRecorder recorder;
    for (const double sample : {1, 2, 3, 4, 5, 6, 7, 8, 9, 10}) {
        recorder.record(sample);
    }
    // 就近秩:p50 = 第 ceil(0.5*10)=5 个,p99 = 第 ceil(9.9)=10 个。
    EXPECT_EQ(recorder.percentile(0.5), 5);
    EXPECT_EQ(recorder.percentile(0.99), 10);
    EXPECT_EQ(recorder.max(), 10);
}

TEST(LatencyRecorderTest, PercentileOfSmallSample) {
    LatencyRecorder recorder;
    for (const double sample : {30, 10, 20}) {
        recorder.record(sample);
    }
    EXPECT_EQ(recorder.percentile(0.5), 20);
    EXPECT_EQ(recorder.percentile(0.99), 30);
}

TEST(PhaseCountersTest, FailuresBreakDownByKindAndMerge) {
    PhaseCounters left;
    left.record_success(1);
    left.record_failure(FailureKind::ConnectionError, 2);
    PhaseCounters right;
    right.record_failure(FailureKind::ConnectionError, 3);
    right.record_failure(FailureKind::AttachRejected, 4);

    left.merge(right);
    EXPECT_EQ(left.attempts, 4U);
    EXPECT_EQ(left.failures, 3U);
    EXPECT_EQ(left.by_kind.at(FailureKind::ConnectionError), 2U);
    EXPECT_EQ(left.by_kind.at(FailureKind::AttachRejected), 1U);
    // 成功与失败都各占一次 attempt 和一次时延样本。
    EXPECT_EQ(left.latency.samples(), 4U);
    EXPECT_EQ(left.latency.summary().max_ms, 4);
}

TEST(LoadgenReportTest, RenderLocksPhaseFieldsAndFailureNames) {
    LoadgenReport report;
    report.robots = 3;
    report.completed = 2;
    report.skipped = 1;
    report.verify.record_success(10);
    report.verify.record_failure(FailureKind::VerifyRejected, 20);
    report.tickets.record_success(30);
    report.attach.record_failure(FailureKind::ConnectionError, 40);

    const std::string rendered = report.render();

    EXPECT_NE(rendered.find("robots: 3  completed: 2\n"), std::string::npos);
    EXPECT_NE(rendered.find("skipped: 1  (窗口关闭时未起跑)\n"),
              std::string::npos);
    EXPECT_NE(
        rendered.find("verify: attempts=2 failures=1 p50_ms=10.000000 "
                      "p99_ms=20.000000 max_ms=20.000000\n"
                      "  verify_rejected: 1\n"),
        std::string::npos);
    EXPECT_NE(
        rendered.find("tickets: attempts=1 failures=0 p50_ms=30.000000 "
                      "p99_ms=30.000000 max_ms=30.000000\n"),
        std::string::npos);
    EXPECT_NE(
        rendered.find("attach: attempts=1 failures=1 p50_ms=40.000000 "
                      "p99_ms=40.000000 max_ms=40.000000\n"
                      "  connection_error: 1\n"),
        std::string::npos);
    // 零 attempt 的相位不占报告行。
    EXPECT_EQ(rendered.find("poll:"), std::string::npos);
    EXPECT_EQ(rendered.find("handoff:"), std::string::npos);
}

TEST(FailureKindNameTest, NamesEveryKind) {
    EXPECT_EQ(failure_kind_name(FailureKind::None), "none");
    EXPECT_EQ(failure_kind_name(FailureKind::VerifyRejected),
              "verify_rejected");
    EXPECT_EQ(failure_kind_name(FailureKind::TicketsRejected),
              "tickets_rejected");
    EXPECT_EQ(failure_kind_name(FailureKind::PollFailed), "poll_failed");
    EXPECT_EQ(failure_kind_name(FailureKind::AdmitTimeout), "admit_timeout");
    EXPECT_EQ(failure_kind_name(FailureKind::AttachRejected),
              "attach_rejected");
    EXPECT_EQ(failure_kind_name(FailureKind::AttachTimeout),
              "attach_timeout");
    EXPECT_EQ(failure_kind_name(FailureKind::HandoffTimeout),
              "handoff_timeout");
    EXPECT_EQ(failure_kind_name(FailureKind::ConnectionError),
              "connection_error");
    EXPECT_EQ(failure_kind_name(FailureKind::HandoffRejected),
              "handoff_rejected");
}

TEST(ParseRobotPhaseTest, AcceptsSpecifiedValues) {
    EXPECT_EQ(parse_robot_phase("verify"), RobotPhase::Verify);
    EXPECT_EQ(parse_robot_phase("tickets"), RobotPhase::Tickets);
    EXPECT_EQ(parse_robot_phase("poll"), RobotPhase::Poll);
    EXPECT_EQ(parse_robot_phase("gateway"), RobotPhase::Gateway);
    EXPECT_EQ(parse_robot_phase("all"), RobotPhase::All);
    EXPECT_EQ(parse_robot_phase("soak"), std::nullopt);
    EXPECT_EQ(parse_robot_phase("gateway_soak"), std::nullopt);
    EXPECT_EQ(parse_robot_phase("full"), std::nullopt);
    EXPECT_EQ(parse_robot_phase("Verify"), std::nullopt);
    EXPECT_EQ(parse_robot_phase(""), std::nullopt);
}

}  // namespace
}  // namespace realm::loadgen
