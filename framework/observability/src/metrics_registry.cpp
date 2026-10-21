#include "realmmesh/observability/metrics_registry.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <stdexcept>
#include <vector>

namespace realm::observability {
namespace {

/// Prometheus 默认直方图桶(秒):覆盖 1ms~10s,延迟桩(~100ms)与
/// 真 DB 拉取都落在档内。
const std::vector<double> default_histogram_buckets = {
    0.005, 0.01, 0.025, 0.05,  0.075, 0.1,  0.25, 0.5,
    0.75,  1.0,  2.5,   5.0,   7.5,   10.0};

/// 转义 Prometheus 标签值中的 \ " 与换行。
[[nodiscard]] std::string escape_label(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        switch (character) {
        case '\\':
            escaped += "\\\\";
            break;
        case '"':
            escaped += "\\\"";
            break;
        case '\n':
            escaped += "\\n";
            break;
        default:
            escaped.push_back(character);
            break;
        }
    }
    return escaped;
}

/// 标签序列规范化:按键排序后拼成 "k=\"v\",..." 的系列区分串。
[[nodiscard]] std::string canonical_labels(
    std::initializer_list<MetricLabel> labels) {
    std::vector<MetricLabel> ordered(labels);
    std::sort(
        ordered.begin(),
        ordered.end(),
        [](const MetricLabel& left, const MetricLabel& right) {
            return left.first < right.first;
        });
    std::string canonical;
    for (const auto& [key, value] : ordered) {
        if (!canonical.empty()) canonical += ',';
        canonical += escape_label(key);
        canonical += "=\"";
        canonical += escape_label(value);
        canonical += '"';
    }
    return canonical;
}

/// 系列键:(指标名, 规范化标签串)。
[[nodiscard]] std::pair<std::string, std::string> series_key(
    std::string_view name,
    std::initializer_list<MetricLabel> labels) {
    return {std::string(name), canonical_labels(labels)};
}

/// 最短往返浮点文本(std::to_chars general 格式):42 → "42"、
/// 0.38 → "0.38",避免固定小数位的噪声零。
[[nodiscard]] std::string format_value(double value) {
    char buffer[32];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    return std::string(buffer, result.ptr);
}

/// 桶上界的 le 文本:最短往返的定点格式(0.0001 不渲染成 1e-04),
/// 亚毫秒桶在抓取端与面板里保持可读。
[[nodiscard]] std::string format_bound(double value) {
    char buffer[64];
    const auto result = std::to_chars(
        buffer, buffer + sizeof(buffer), value, std::chars_format::fixed);
    return std::string(buffer, result.ptr);
}

/// 样本行:<name>{<labels>} <value>;无标签省略花括号。
void append_sample(
    std::string& output,
    std::string_view name,
    const std::string& labels,
    std::string_view value) {
    output += name;
    if (!labels.empty()) {
        output += '{';
        output += labels;
        output += '}';
    }
    output += ' ';
    output += value;
    output += '\n';
}

/// 系列在首次观测时拷入所属指标名的桶上界,之后布局不再变化。
struct Histogram final {
    std::vector<double> upper_bounds;
    std::vector<std::uint64_t> buckets;
    double sum{0.0};
    std::uint64_t count{0};
};

}  // namespace

class MetricsRegistry::Impl final {
public:
    /// (指标名, 标签串) → 值;有序 map 使渲染按名排序、家族内按标签
    /// 排序,输出确定。
    std::map<std::pair<std::string, std::string>, double> counters;
    std::map<std::pair<std::string, std::string>, double> gauges;
    std::map<std::pair<std::string, std::string>, Histogram> histograms;
    /// 指标名 → 自定义桶上界;未登记的名字用默认桶。
    std::map<std::string, std::vector<double>, std::less<>> bucket_layouts;
    mutable std::shared_mutex mutex;
};

MetricsRegistry::MetricsRegistry() : impl_(std::make_unique<Impl>()) {}

MetricsRegistry::~MetricsRegistry() = default;

void MetricsRegistry::counter_add(
    std::string_view name,
    double value,
    std::initializer_list<MetricLabel> labels) {
    const auto key = series_key(name, labels);
    std::unique_lock lock(impl_->mutex);
    impl_->counters[key] += value;
}

void MetricsRegistry::counter_set(
    std::string_view name,
    double value,
    std::initializer_list<MetricLabel> labels) {
    const auto key = series_key(name, labels);
    std::unique_lock lock(impl_->mutex);
    impl_->counters[key] = value;
}

void MetricsRegistry::gauge_set(
    std::string_view name,
    double value,
    std::initializer_list<MetricLabel> labels) {
    const auto key = series_key(name, labels);
    std::unique_lock lock(impl_->mutex);
    impl_->gauges[key] = value;
}

void MetricsRegistry::histogram_buckets(
    std::string_view name, std::initializer_list<double> upper_bounds) {
    std::vector<double> bounds(upper_bounds);
    if (bounds.empty()) {
        throw std::invalid_argument("histogram buckets must not be empty");
    }
    for (std::size_t i = 0; i < bounds.size(); ++i) {
        if (!std::isfinite(bounds[i]) ||
            (i > 0 && !(bounds[i - 1] < bounds[i]))) {
            throw std::invalid_argument(
                "histogram buckets must be finite and strictly increasing");
        }
    }
    std::unique_lock lock(impl_->mutex);
    if (const auto existing = impl_->bucket_layouts.find(name);
        existing != impl_->bucket_layouts.end()) {
        if (existing->second == bounds) return;
        throw std::logic_error(
            "histogram buckets already registered: " + std::string(name));
    }
    // 名字是 (名, 标签) 键的前缀:lower_bound 落到该名首个系列。
    const auto observed = impl_->histograms.lower_bound({std::string(name), {}});
    if (observed != impl_->histograms.end() &&
        observed->first.first == name) {
        throw std::logic_error(
            "histogram already observed before bucket registration: " +
            std::string(name));
    }
    impl_->bucket_layouts.emplace(std::string(name), std::move(bounds));
}

void MetricsRegistry::histogram_observe(
    std::string_view name,
    double value,
    std::initializer_list<MetricLabel> labels) {
    const auto key = series_key(name, labels);
    std::unique_lock lock(impl_->mutex);
    auto& histogram = impl_->histograms[key];
    if (histogram.upper_bounds.empty()) {
        const auto layout = impl_->bucket_layouts.find(name);
        histogram.upper_bounds = layout != impl_->bucket_layouts.end()
                                     ? layout->second
                                     : default_histogram_buckets;
        histogram.buckets.assign(histogram.upper_bounds.size(), 0);
    }
    for (std::size_t i = 0; i < histogram.upper_bounds.size(); ++i) {
        if (value <= histogram.upper_bounds[i]) {
            ++histogram.buckets[i];
        }
    }
    histogram.sum += value;
    ++histogram.count;
}

std::string MetricsRegistry::render() const {
    std::shared_lock lock(impl_->mutex);
    // 拷贝出快照再渲染,锁内只做取值。
    const auto counters = impl_->counters;
    const auto gauges = impl_->gauges;
    const auto histograms = impl_->histograms;
    lock.unlock();

    // 家族(同名系列)各拼好样本行,再按指标名统一排序输出:文本格式
    // 没有分段概念,全部家族按名序是抓取端的惯例形态。
    struct Family {
        std::string name;
        std::string type;
        std::string samples;
    };
    std::vector<Family> families;
    // (名, 标签) 有序 map 使同名系列相邻:名字切换处即家族边界。
    auto collect_scalar =
        [&](const auto& series, std::string_view type) {
            for (auto it = series.begin(); it != series.end();) {
                const auto& name = it->first.first;
                Family family{std::string(name), std::string(type), {}};
                for (; it != series.end() && it->first.first == name; ++it) {
                    append_sample(
                        family.samples,
                        it->first.first,
                        it->first.second,
                        format_value(it->second));
                }
                families.push_back(std::move(family));
            }
        };
    collect_scalar(counters, "counter");
    collect_scalar(gauges, "gauge");
    for (const auto& [key, histogram] : histograms) {
        Family family{key.first, "histogram", {}};
        const auto& name = key.first;
        const auto& labels = key.second;
        for (std::size_t i = 0; i < histogram.upper_bounds.size(); ++i) {
            const auto bound = format_bound(histogram.upper_bounds[i]);
            append_sample(
                family.samples,
                name + "_bucket",
                labels.empty() ? "le=\"" + bound + "\""
                               : labels + ",le=\"" + bound + "\"",
                format_value(static_cast<double>(histogram.buckets[i])));
        }
        append_sample(
            family.samples,
            name + "_bucket",
            labels.empty() ? "le=\"+Inf\"" : labels + ",le=\"+Inf\"",
            format_value(static_cast<double>(histogram.count)));
        append_sample(
            family.samples, name + "_sum", labels, format_value(histogram.sum));
        append_sample(
            family.samples,
            name + "_count",
            labels,
            format_value(static_cast<double>(histogram.count)));
        families.push_back(std::move(family));
    }

    std::sort(
        families.begin(),
        families.end(),
        [](const Family& left, const Family& right) {
            return left.name < right.name;
        });
    std::string output;
    for (const auto& family : families) {
        output += "# TYPE ";
        output += family.name;
        output += ' ';
        output += family.type;
        output += '\n';
        output += family.samples;
    }
    return output;
}

}  // namespace realm::observability
