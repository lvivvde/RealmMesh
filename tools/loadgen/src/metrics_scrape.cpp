#include "realmmesh/loadgen/metrics_scrape.hpp"

#include "realmmesh/loadgen/http_client.hpp"

#include <string>

namespace realm::loadgen {

double MetricsSnapshot::total(std::string_view name) const {
    double sum = 0;
    for (const auto& [key, value] : series) {
        if (key == name ||
            (key.size() > name.size() && key.starts_with(name) &&
             key[name.size()] == '{')) {
            sum += value;
        }
    }
    return sum;
}

bool MetricsSnapshot::has(std::string_view name) const {
    for (const auto& [key, value] : series) {
        if (key == name ||
            (key.size() > name.size() && key.starts_with(name) &&
             key[name.size()] == '{')) {
            return true;
        }
    }
    return false;
}

void parse_metrics_line(std::string_view line, MetricsSnapshot& into) {
    while (!line.empty() && line.front() == ' ') {
        line.remove_prefix(1);
    }
    if (line.empty() || line.front() == '#') {
        return;
    }
    std::string_view name = line;
    std::string key;
    const auto brace = line.find('{');
    if (brace != std::string_view::npos) {
        const auto closing = line.find('}', brace);
        if (closing == std::string_view::npos) {
            return;
        }
        name = line.substr(0, brace);
        key = std::string{name};
        key.append(line.substr(brace, closing - brace + 1));
        line = line.substr(closing + 1);
    } else {
        const auto space = line.find_first_of(" \t");
        if (space == std::string_view::npos) {
            return;
        }
        name = line.substr(0, space);
        key = std::string{name};
        line = line.substr(space);
    }
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
        line.remove_prefix(1);
    }
    const auto value_end = line.find_first_of(" \t\r");
    if (value_end != std::string_view::npos) {
        line = line.substr(0, value_end);
    }
    if (line.empty()) {
        return;
    }
    // 值为 double 文本;畸形数字行忽略(诊断工具,不抛)。
    try {
        std::size_t parsed = 0;
        const double value = std::stod(std::string{line}, &parsed);
        if (parsed == line.size()) {
            into.series[std::move(key)] = value;
        }
    } catch (const std::exception&) {
    }
}

std::optional<MetricsSnapshot> scrape_metrics(
    const ServiceAddress& address,
    std::chrono::steady_clock::time_point deadline) {
    auto connection = TlsHttpConnection::dial(address, deadline);
    if (connection == nullptr) {
        return std::nullopt;
    }
    auto response = connection->request(
        "GET", "/metrics", std::nullopt, "", deadline);
    if (!response.has_value() || response->status != 200) {
        return std::nullopt;
    }
    MetricsSnapshot snapshot;
    std::string_view body{response->body};
    while (!body.empty()) {
        const auto newline = body.find('\n');
        parse_metrics_line(body.substr(0, newline), snapshot);
        body = newline == std::string_view::npos
                   ? std::string_view{}
                   : body.substr(newline + 1);
    }
    return snapshot;
}

}  // namespace realm::loadgen
