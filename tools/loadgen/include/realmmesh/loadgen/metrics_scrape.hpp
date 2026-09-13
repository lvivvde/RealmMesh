#pragma once

#include "realmmesh/loadgen/stats.hpp"

#include <chrono>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace realm::loadgen {

/// 一次 /metrics 抓取的快照:全序列键(name{labels})→ 值。
/// total(name) 聚合同名序列(跨标签求和,无标签序列直接取)。
struct MetricsSnapshot final {
    std::map<std::string, double> series;

    [[nodiscard]] double total(std::string_view name) const;
    [[nodiscard]] bool has(std::string_view name) const;
};

/// 抓取一个 HTTPS 服务的 /metrics;网络层失败返回 nullopt。
[[nodiscard]] std::optional<MetricsSnapshot> scrape_metrics(
    const ServiceAddress& address,
    std::chrono::steady_clock::time_point deadline);

/// Prometheus 文本 exposition 解析(供单测):跳过 '#',逐行取
/// name{labels} value;畸形行忽略。
void parse_metrics_line(std::string_view line, MetricsSnapshot& into);

}  // namespace realm::loadgen
