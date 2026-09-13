/// 告警 rules 与 Grafana 仪表盘的指标名一致性校验(#47):抽取
/// deploy/observability 下 prometheus rules 的 expr 与仪表盘 JSON 的
/// panel targets,断言引用的指标名 ⊆ 代码暴露的指标名清单。改名若不
/// 同步规则/仪表盘即在此失败,不会静默弄瞎告警。各指标名的外显行为
/// 由各服务测试断言实际渲染行(见 login_verify/queue/帧级测试)。

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace realm::service_host {
namespace {

#ifndef REALMMESH_SOURCE_DIR
#error "REALMMESH_SOURCE_DIR must be defined"
#endif

[[nodiscard]] std::string read_file(const std::string& path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    return std::string{std::istreambuf_iterator<char>(stream),
                       std::istreambuf_iterator<char>()};
}

/// 从 rules YAML 抽取 expr 值:单行取冒号后文本;`>-` 块取更深缩进的
/// 连续行(遇同层/更浅缩进止,该行回退参与键检测)。注释行与
/// annotations 不参与抽取。
[[nodiscard]] std::vector<std::string> extract_exprs(
    const std::string& text) {
    std::vector<std::string> exprs;
    std::istringstream stream{text};
    std::string line;
    std::size_t expr_indent = 0;
    bool in_block = false;
    while (std::getline(stream, line)) {
        const auto first = line.find_first_not_of(' ');
        if (first == std::string::npos) {
            continue;
        }
        if (in_block) {
            if (first > expr_indent) {
                exprs.back() += " " + line.substr(first);
                continue;
            }
            in_block = false;
        }
        // expr: 键可能带列表前缀 "- "。
        std::size_t key_at = std::string::npos;
        std::size_t probe = first;
        if (line.compare(probe, 2, "- ") == 0) {
            probe += 2;
        }
        if (line.compare(probe, 5, "expr:") == 0) {
            key_at = probe;
        }
        if (key_at == std::string::npos) {
            continue;
        }
        expr_indent = key_at;
        const auto value = line.substr(key_at + 5);
        const auto trimmed = value.substr(value.find_first_not_of(" "));
        if (trimmed == ">-" || trimmed == ">" || trimmed == "|") {
            in_block = true;
            exprs.emplace_back();
        } else if (!trimmed.empty()) {
            exprs.push_back(trimmed);
        }
    }
    return exprs;
}

/// PromQL 关键字/函数与标签名:抽取时剔除(先删引号内文本再取标识符)。
constexpr std::string_view kNonMetrics[] = {
    "abs",        "absent",       "absent_over_time", "avg",
    "avg_over_time", "bool",      "bottomk",          "by",
    "ceil",       "changes",      "clamp_max",        "clamp_min",
    "count",      "count_over_time", "delta",        "deriv",
    "exp",        "floor",        "group_left",       "group_right",
    "histogram_quantile", "idelta", "ignore",         "ignoring",
    "increase",   "irate",        "job",              "instance",
    "label_join", "label_replace", "le",              "ln",
    "log10",      "log2",         "max",              "max_over_time",
    "min",        "min_over_time", "offset",          "on",
    "or",         "and",          "unless",           "present_over_time",
    "quantile",   "rate",         "resets",           "round",
    "scalar",     "sort",         "sort_desc",        "sqrt",
    "stddev",     "stdvar",       "sum",              "sum_over_time",
    "time",       "topk",         "vector",           "without",
    "kind",       "reason",       "stage",            "service_name",
    "job_name",
};

[[nodiscard]] bool is_known(
    const std::string& name,
    const std::set<std::string>& known) {
    static constexpr std::string_view histogram_suffixes[] = {
        "_bucket", "_count", "_sum"};
    if (known.count(name) != 0U) {
        return true;
    }
    for (const auto suffix : histogram_suffixes) {
        if (name.ends_with(suffix) &&
            known.count(name.substr(0, name.size() - suffix.size())) != 0U) {
            return true;
        }
    }
    return false;
}

/// 已暴露指标清单:代码真实渲染的指标名(#34 清单接线后的形态 +
/// 既有日志管道/就绪指标,与各服务测试断言的渲染行一致)。
[[nodiscard]] std::set<std::string> exposed_metrics() {
    return {
        // 健全服(#34)
        "verify_requests_total",
        "verify_reject_total",
        "verify_issue_duration_seconds",
        // 排队调度服(#34)
        "tickets_issued_total",
        "released_number",
        "admit_rate",
        "queue_length_est",
        "progress_requests_total",
        "admit_batches_total",
        // 网关(#34;budget 编码为 edge_budget{kind},fetch/jti 带 edge_
        // 前缀——/metrics 按服务隔离,见 #47 解决评论)
        "edge_sessions",
        "edge_budget",
        "edge_fetch_retry_total",
        "edge_fetch_duration_seconds",
        "edge_jti_replay_rejected_total",
        // 既有暴露(LoggerMetricsServer / ServiceHost)
        "realmmesh_service_ready",
        "realmmesh_log_events_accepted_total",
        "realmmesh_log_events_emitted_total",
        "realmmesh_log_events_dropped_total",
        "realmmesh_log_events_rejected_total",
        "realmmesh_log_events_truncated_total",
        "realmmesh_log_write_errors_total",
        "realmmesh_log_events_sampled_out_total",
        "realmmesh_log_queue_dropped_total",
        "realmmesh_log_normal_queue_size",
    };
}

[[nodiscard]] std::set<std::string> allowlisted_metrics() {
    return {
        // Prometheus 内建
        "up",
        // realm 侧接线属 #46 后续:仪表盘占位引用,接线前显示为空
        "enter_realm_total",
        "enter_realm_rejected_total",
        "sessions_current",
    };
}

void assert_refs_known(
    const std::vector<std::string>& exprs,
    std::string_view origin) {
    const auto known = exposed_metrics();
    const auto allowlist = allowlisted_metrics();
    static const std::regex quoted{"\"[^\"]*\""};
    // 区间/时长选择器([5m] 等)与标签匹配器({k=v})内不会出现指标名,
    // 先行剔除,避免把单位字母等误当指标。
    static const std::regex selector{"\\[[^\\]]*\\]"};
    static const std::regex matcher{"\\{[^}]*\\}"};
    static const std::regex identifier{
        "[a-zA-Z_:][a-zA-Z0-9_:]*"};
    for (const auto& expr : exprs) {
        const auto bare = std::regex_replace(
            std::regex_replace(std::regex_replace(expr, quoted, ""),
                               matcher, ""),
            selector, "");
        for (std::sregex_iterator it{
                 bare.begin(), bare.end(), identifier};
             it != std::sregex_iterator{}; ++it) {
            const std::string name = it->str();
            bool non_metric = false;
            for (const auto keyword : kNonMetrics) {
                if (name == keyword) {
                    non_metric = true;
                    break;
                }
            }
            if (non_metric) {
                continue;
            }
            if (!is_known(name, known) && allowlist.count(name) == 0U) {
                ADD_FAILURE()
                    << origin << ": expr 引用了未暴露的指标名 \"" << name
                    << "\"\n  expr: " << expr;
            }
        }
    }
}

TEST(ObservabilityArtifactsTest, AlertRulesReferenceExposedMetrics) {
    const std::string rules = read_file(
        std::string{REALMMESH_SOURCE_DIR} +
        "/deploy/observability/prometheus/rules/realmmesh.yaml");
    ASSERT_FALSE(rules.empty());
    // 六条 v1 告警(#34 定案)全部在册。
    static const std::regex alert{"alert:\\s*([A-Za-z0-9_]+)"};
    std::set<std::string> alerts;
    for (std::sregex_iterator it{rules.begin(), rules.end(), alert};
         it != std::sregex_iterator{}; ++it) {
        alerts.insert((*it)[1].str());
    }
    ASSERT_EQ(alerts.size(), 6U)
        << "expected 6 v1 alerts, got " << alerts.size();
    assert_refs_known(extract_exprs(rules), "rules/realmmesh.yaml");
}

TEST(ObservabilityArtifactsTest, DashboardsReferenceExposedMetrics) {
    static constexpr std::string_view dashboards[] = {
        "login-verify.json", "queue.json",     "gateway.json",
        "realm.json",        "platform.json",  "surge-overview.json",
    };
    const auto known = exposed_metrics();
    const auto allowlist = allowlisted_metrics();
    for (const auto name : dashboards) {
        const std::string path =
            std::string{REALMMESH_SOURCE_DIR} +
            "/deploy/observability/grafana/dashboards/" + std::string{name};
        const std::string text = read_file(path);
        ASSERT_FALSE(text.empty());
        const auto document = nlohmann::json::parse(
            text, nullptr, /*allow_exceptions=*/false);
        ASSERT_FALSE(document.is_discarded()) << path << " is not valid JSON";
        ASSERT_TRUE(document.contains("panels"));
        std::vector<std::string> exprs;
        for (const auto& panel : document["panels"]) {
            if (!panel.contains("targets")) {
                continue;
            }
            for (const auto& target : panel["targets"]) {
                if (target.contains("expr")) {
                    exprs.push_back(target["expr"].get<std::string>());
                }
            }
        }
        assert_refs_known(exprs, std::string{name});
    }
}

TEST(ObservabilityArtifactsTest, DashboardsPinPrometheusDatasource) {
    static constexpr std::string_view dashboards[] = {
        "login-verify.json", "queue.json",    "gateway.json",
        "realm.json",        "platform.json", "surge-overview.json",
    };
    for (const auto name : dashboards) {
        const auto document = nlohmann::json::parse(
            read_file(
                std::string{REALMMESH_SOURCE_DIR} +
                "/deploy/observability/grafana/dashboards/" +
                std::string{name}),
            nullptr, /*allow_exceptions=*/false);
        ASSERT_FALSE(document.is_discarded());
        for (const auto& panel : document["panels"]) {
            for (const auto& target : panel["targets"]) {
                ASSERT_EQ(
                    target["datasource"]["uid"].get<std::string>(),
                    "realmmesh-prometheus")
                    << name << " panel " << panel["title"].get<std::string>();
            }
        }
    }
}

}  // namespace
}  // namespace realm::service_host
