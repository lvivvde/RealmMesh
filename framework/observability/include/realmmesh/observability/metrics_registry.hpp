#pragma once

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace realm::observability {

/// 标签键值对(键序由注册表规范化,渲染输出确定)。
using MetricLabel = std::pair<std::string_view, std::string_view>;

/// 极小 Prometheus 指标注册表(#47):线程安全的 counter/gauge/histogram
/// 记账与文本格式渲染。只做计数与渲染,不做推送、不建连接——上报通道
/// 仍是既有 MetricsServer。指标名不加服务前缀:/metrics 端点按服务隔离。
/// counter 两态:counter_add 事件累计;counter_set 轮询发布累计源
/// (下调按 Prometheus 语义视为 reset)。histogram 固定默认桶
/// (0.005~10,+Inf)。渲染按指标名排序、家族内按标签序列排序,输出确定。
class MetricsRegistry final {
public:
    MetricsRegistry();
    ~MetricsRegistry();

    MetricsRegistry(const MetricsRegistry&) = delete;
    MetricsRegistry& operator=(const MetricsRegistry&) = delete;

    /// counter 系列值累计 += value(事件驱动路径)。
    void counter_add(
        std::string_view name,
        double value = 1.0,
        std::initializer_list<MetricLabel> labels = {});
    /// counter 系列值直接设定(轮询发布累计源路径)。
    void counter_set(
        std::string_view name,
        double value,
        std::initializer_list<MetricLabel> labels = {});
    /// gauge 系列值直接设定。
    void gauge_set(
        std::string_view name,
        double value,
        std::initializer_list<MetricLabel> labels = {});
    /// histogram 累计一次观测。
    void histogram_observe(
        std::string_view name,
        double value,
        std::initializer_list<MetricLabel> labels = {});

    /// Prometheus 文本格式快照(# TYPE 行 + 样本行)。
    [[nodiscard]] std::string render() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace realm::observability
