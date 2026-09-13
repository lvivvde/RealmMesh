#include "realmmesh/loadgen/loadgen.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <semaphore>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace realm::loadgen {

std::string LoadgenReport::render() const {
    std::string text;
    text.reserve(512);
    text += "loadgen report\n";
    text += "robots: " + std::to_string(robots) +
            "  completed: " + std::to_string(completed) + "\n";

    auto render_phase = [&text](std::string_view name,
                                const PhaseCounters& counters) {
        if (counters.attempts == 0) {
            return;
        }
        const auto summary = counters.latency.summary();
        text += std::string{name};
        text += ": attempts=" + std::to_string(counters.attempts) +
                " failures=" + std::to_string(counters.failures) +
                " p50_ms=" + std::to_string(summary.p50_ms) +
                " p99_ms=" + std::to_string(summary.p99_ms) +
                " max_ms=" + std::to_string(summary.max_ms) + "\n";
        for (const auto& [kind, count] : counters.by_kind) {
            text += "  " + std::string{failure_kind_name(kind)} +
                    ": " + std::to_string(count) + "\n";
        }
    };
    render_phase("verify", verify);
    render_phase("tickets", tickets);
    render_phase("poll", poll);
    render_phase("attach", attach);
    render_phase("handoff", handoff);

    if (service_metrics.has_value()) {
        text += "service metrics:\n";
        auto metric = [&text](const MetricsSnapshot& snapshot,
                              std::string_view name) {
            if (snapshot.has(name)) {
                text += "  " + std::string{name} + ": " +
                        std::to_string(snapshot.total(name)) + "\n";
            }
        };
        metric(*service_metrics, "verify_requests_total");
        metric(*service_metrics, "verify_reject_total");
        metric(*service_metrics, "tickets_issued_total");
        metric(*service_metrics, "progress_requests_total");
        metric(*service_metrics, "edge_sessions");
        metric(*service_metrics, "edge_fetch_retry_total");
        metric(*service_metrics, "edge_fetch_duration_seconds_count");
        metric(*service_metrics, "edge_jti_replay_rejected_total");
        // 拉取失败率 = retry/(retry+count),#34 口径的门槛判定直读数。
        const auto fetch_retry =
            service_metrics->total("edge_fetch_retry_total");
        const auto fetch_done =
            service_metrics->total("edge_fetch_duration_seconds_count");
        if (fetch_retry + fetch_done > 0) {
            text += "  fetch_failure_rate: " +
                    std::to_string(fetch_retry / (fetch_retry + fetch_done)) +
                    "\n";
        }
    }
    return text;
}

LoadgenReport run_loadgen(const LoadgenConfig& config) {
    LoadgenReport report;
    report.robots = config.robots;

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(config.duration_seconds));

    std::mutex report_mutex;
    std::counting_semaphore<> slots{
        static_cast<std::ptrdiff_t>(
            config.concurrency == 0 ? 1 : config.concurrency)};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(config.robots));

    for (std::uint64_t index = 0; index < config.robots; ++index) {
        workers.emplace_back([&, index] {
            // 爬坡错峰:第 i 个机器人延迟 i*ramp/robots 起跑。
            if (config.ramp_seconds > 0 && config.robots > 1) {
                const auto delay = std::chrono::duration_cast<
                    std::chrono::steady_clock::duration>(
                    std::chrono::duration<double>(
                        config.ramp_seconds *
                        static_cast<double>(index) /
                        static_cast<double>(config.robots - 1)));
                std::this_thread::sleep_for(delay);
            }
            RobotOptions options;
            options.phase = config.phase;
            options.endpoints = config.endpoints;
            options.account = config.account_prefix + "-" +
                std::to_string(config.accounts == 0
                                   ? index
                                   : index % config.accounts);
            options.credential = config.credential;
            options.poll_interval = config.poll_interval;
            options.deadline = deadline;
            if (config.phase == RobotPhase::All) {
                // soak 水位:handed-off 会话保持到总截止。
                options.hold_until = deadline;
            }
            options.collect_artifacts = config.collect_number_tokens;
            slots.acquire();

            PhaseCounters verify;
            PhaseCounters tickets;
            PhaseCounters poll;
            PhaseCounters attach;
            PhaseCounters handoff;
            auto outcome = run_robot(
                options,
                RobotCounters{verify, tickets, poll, attach, handoff});

            std::scoped_lock lock{report_mutex};
            report.verify.merge(verify);
            report.tickets.merge(tickets);
            report.poll.merge(poll);
            report.attach.merge(attach);
            report.handoff.merge(handoff);
            if (outcome.completed) {
                ++report.completed;
            }
            if (config.collect_number_tokens &&
                !outcome.number_token.empty()) {
                report.number_tokens.push_back(
                    std::move(outcome.number_token));
            }
            slots.release();
        });
    }
    for (auto& worker : workers) {
        worker.join();
    }

    if (config.metrics_endpoint.has_value()) {
        report.service_metrics = scrape_metrics(
            *config.metrics_endpoint,
            std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
    return report;
}

}  // namespace realm::loadgen
