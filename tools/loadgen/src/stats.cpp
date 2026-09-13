#include "realmmesh/loadgen/stats.hpp"

#include <algorithm>
#include <cmath>

namespace realm::loadgen {

std::string_view failure_kind_name(FailureKind kind) {
    switch (kind) {
    case FailureKind::None:
        return "none";
    case FailureKind::VerifyRejected:
        return "verify_rejected";
    case FailureKind::TicketsRejected:
        return "tickets_rejected";
    case FailureKind::PollFailed:
        return "poll_failed";
    case FailureKind::AdmitTimeout:
        return "admit_timeout";
    case FailureKind::AttachRejected:
        return "attach_rejected";
    case FailureKind::AttachTimeout:
        return "attach_timeout";
    case FailureKind::HandoffTimeout:
        return "handoff_timeout";
    case FailureKind::ConnectionError:
        return "connection_error";
    case FailureKind::RobotTimeout:
        return "robot_timeout";
    }
    return "unknown";
}

void LatencyRecorder::record(double milliseconds) {
    samples_.push_back(milliseconds);
}

double LatencyRecorder::percentile(double p) const {
    if (samples_.empty()) {
        return 0;
    }
    // 就近秩:第 ceil(p*n) 个样本(1 起),与 prometheus 分位语义同向。
    auto sorted = samples_;
    std::sort(sorted.begin(), sorted.end());
    const auto rank = static_cast<std::size_t>(
        std::ceil(p * static_cast<double>(sorted.size())));
    return sorted[std::min(rank, sorted.size()) - 1];
}

double LatencyRecorder::max() const {
    return samples_.empty()
               ? 0
               : *std::max_element(samples_.begin(), samples_.end());
}

std::uint64_t LatencyRecorder::samples() const {
    return samples_.size();
}

LatencyRecorder::Summary LatencyRecorder::summary() const {
    return {.samples = samples(),
            .p50_ms = percentile(0.5),
            .p99_ms = percentile(0.99),
            .max_ms = max()};
}

void LatencyRecorder::merge(const LatencyRecorder& other) {
    samples_.insert(
        samples_.end(), other.samples_.begin(), other.samples_.end());
}

void PhaseCounters::record_success(double milliseconds) {
    ++attempts;
    latency.record(milliseconds);
}

void PhaseCounters::record_failure(FailureKind kind, double milliseconds) {
    ++attempts;
    ++failures;
    latency.record(milliseconds);
    ++by_kind[kind];
}

void PhaseCounters::merge(const PhaseCounters& other) {
    attempts += other.attempts;
    failures += other.failures;
    for (const auto& [kind, count] : other.by_kind) {
        by_kind[kind] += count;
    }
    latency.merge(other.latency);
}

}  // namespace realm::loadgen
