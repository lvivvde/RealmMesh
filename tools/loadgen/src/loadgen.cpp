#include "realmmesh/loadgen/loadgen.hpp"

#include "realmmesh/loadgen/login_chain_metrics.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace realm::loadgen {

std::string LoadgenReport::render() const {
    std::string text;
    text.reserve(512);
    text += "loadgen report\n";
    text += "robots: " + std::to_string(robots) +
            "  completed: " + std::to_string(completed) + "\n";
    if (skipped > 0) {
        text += "skipped: " + std::to_string(skipped) +
                "  (窗口关闭时未起跑)\n";
    }

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

    // 拨号失败分型(仅在有失败时占行):connection_error 只说"网络层
    // 失败",细分计数把内核层原因(端口不可用/拒连/握手中断)带出来。
    const auto& dial = dial_diagnostics;
    const auto dial_total = dial.resolve.load() + dial.socket.load() +
        dial.configure.load() + dial.connect.load() +
        dial.connect_timeout.load() + dial.ssl_setup.load() +
        dial.handshake.load() + dial.cancelled.load();
    if (dial_total > 0) {
        text += "dial_failures:";
        auto append = [&text](std::string_view name, std::uint64_t count) {
            if (count > 0) {
                text += "  " + std::string{name} + "=" +
                        std::to_string(count);
            }
        };
        append("getaddrinfo", dial.resolve.load());
        append("socket", dial.socket.load());
        append("fcntl", dial.configure.load());
        append("connect", dial.connect.load());
        append("connect_poll_timeout", dial.connect_timeout.load());
        append("ssl_setup", dial.ssl_setup.load());
        append("ssl_connect", dial.handshake.load());
        append("cancelled", dial.cancelled.load());
        text += "  last_errno=" + std::to_string(dial.last_errno.load()) +
                "\n";
    }

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

/// 异常安全收尾:unwind 时先 join 全部工作线程,再让异常继续传播。
/// vector<thread> 若带着 joinable 线程析构是裸 terminate,会把真正
/// 的异常类型吞掉(实测 CI 上只剩一行 libc++abi: terminating)。
struct ThreadJoiner final {
    std::vector<std::thread>& threads;

    void join() {
        for (auto& worker : threads) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    ~ThreadJoiner() { join(); }
};

[[nodiscard]] LoadgenLoginOptions login_options_for(
    const LoadgenConfig& config,
    std::string account,
    client::TimePoint deadline) {
    LoadgenLoginOptions options;
    options.target = config.target;
    options.polling = config.polling;
    options.endpoints = config.endpoints;
    options.account = std::move(account);
    options.credential = config.credential;
    options.poll_interval = config.poll_interval;
    options.deadline = deadline;
    if (config.target == LoadgenLoginTarget::GatewaySoak) {
        options.hold_until = deadline;
    }
    return options;
}

void validate_login_config(const LoadgenConfig& config) {
    const auto horizon = client::TimePoint::max();
    auto validation = adapt_login_run(
        login_options_for(config, config.account_prefix + "-0", horizon));
    if (std::holds_alternative<LoginRunConfigError>(validation)) {
        throw std::invalid_argument("invalid loadgen Login Chain configuration");
    }
}

LoadgenReport run_loadgen(const LoadgenConfig& config) {
    validate_login_config(config);
    LoadgenReport report;
    report.robots = config.robots;

    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started +
        std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double>(config.duration_seconds));

    std::mutex report_mutex;
    // 线程数 = 并发槽数,机器人从原子游标依次领号,而非一机器人一线程:
    // 万号档一机器人一线程会把上万线程压进调度器(本地实测 M3 全程
    // 烧掉数百 CPU 秒系统态),macOS 还有每进程线程数上限,线程创建
    // 失败抛 system_error 会在 unwind 里裸 terminate。
    const auto slot_count = static_cast<std::uint64_t>(
        config.concurrency == 0 ? 1 : config.concurrency);
    const auto worker_count = std::min(slot_count, config.robots);
    std::atomic<std::uint64_t> next_robot{0};
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(worker_count));
    ThreadJoiner joiner{workers};

    for (std::uint64_t slot = 0; slot < worker_count; ++slot) {
        workers.emplace_back([&] {
            for (;;) {
                const std::uint64_t index =
                    next_robot.fetch_add(1, std::memory_order_relaxed);
                if (index >= config.robots) {
                    return;
                }
                // 爬坡错峰:第 i 个机器人不早于 started+i*ramp/robots
                // 起跑;落在窗口之外的不再空等。
                if (config.ramp_seconds > 0 && config.robots > 1) {
                    const auto delay = std::chrono::duration_cast<
                        std::chrono::steady_clock::duration>(
                        std::chrono::duration<double>(
                            config.ramp_seconds *
                            static_cast<double>(index) /
                            static_cast<double>(config.robots - 1)));
                    if (started + delay >= deadline) {
                        std::scoped_lock lock{report_mutex};
                        ++report.skipped;
                        continue;
                    }
                    std::this_thread::sleep_until(started + delay);
                }
                if (std::chrono::steady_clock::now() >= deadline) {
                    // 窗口已关:没起跑的记 skipped,不伪造链路失败。
                    std::scoped_lock lock{report_mutex};
                    ++report.skipped;
                    continue;
                }

                PhaseCounters verify;
                PhaseCounters tickets;
                PhaseCounters poll;
                PhaseCounters attach;
                PhaseCounters handoff;

                auto options = login_options_for(
                    config,
                    config.account_prefix + "-" +
                        std::to_string(config.accounts == 0
                                           ? index
                                           : index % config.accounts),
                    deadline);
                auto adaptation = adapt_login_run(options);
                auto* adapted = std::get_if<AdaptedLoginRun>(&adaptation);
                if (adapted == nullptr) {
                    // 起跑检查与 Adapter 构造间窗口刚好关闭：这类机器人
                    // 没发出网络动作，沿用 skipped 口径。
                    std::scoped_lock lock{report_mutex};
                    ++report.skipped;
                    continue;
                }

                client::WireEnterRealmRedeemer redeemer;
                client::WireLoginTransport wire(
                    adapted->wire, redeemer, adapted->transport);
                MetricsLoginChainTransport measured(
                    wire, {verify, tickets, poll, attach, handoff});
                client::LoginChain chain(measured, std::move(adapted->chain));
                auto result = chain.run(std::move(adapted->run));

                // AdmitTimeout 是状态机内部的终态，不来自某次端口调用；
                // 显式补进旧 poll 失败分型，保持报告含义。
                if (const auto* failure = result.failure();
                    failure != nullptr &&
                    failure->reason == client::ChainFailure::AdmitTimeout) {
                    poll.record_failure(FailureKind::AdmitTimeout, 0.0);
                }
                std::string number_token;
                if (config.collect_number_tokens) {
                    number_token = measured.last_number_token();
                }

                std::scoped_lock lock{report_mutex};
                report.verify.merge(verify);
                report.tickets.merge(tickets);
                report.poll.merge(poll);
                report.attach.merge(attach);
                report.handoff.merge(handoff);
                if (result.succeeded()) {
                    ++report.completed;
                }
                if (config.collect_number_tokens &&
                    !number_token.empty()) {
                    report.number_tokens.push_back(std::move(number_token));
                }
            }
        });
    }
    // 正常路径先 join 再抓服务指标/返回报告；异常路径由析构兜底。
    joiner.join();

    if (config.metrics_endpoint.has_value()) {
        report.service_metrics = scrape_metrics(
            *config.metrics_endpoint,
            std::chrono::steady_clock::now() + std::chrono::seconds{5});
    }
    return report;
}

}  // namespace realm::loadgen
