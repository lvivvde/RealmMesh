#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <source_location>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>

namespace realm::observability {

/// 日志严重级别,数值越大越严重,用于级别过滤与优先级队列判定。
enum class Severity : std::uint8_t {
    Trace,  ///< 最详细的跟踪信息,仅本地调试使用
    Debug,  ///< 调试信息
    Info,   ///< 常规运行信息(默认最低级别)
    Warn,   ///< 警告,进入优先队列异步写出
    Error,  ///< 错误,进入优先队列异步写出
    Fatal,  ///< 致命错误,通常伴随进程终止
};

/// 字段数据分类标签,随日志一同输出,便于后续数据合规处理。
enum class DataClass : std::uint8_t {
    Public,        ///< 可对外公开的数据
    Internal,      ///< 仅内部可见(默认)
    Pseudonymous,  ///< 已假名化处理的用户相关数据
};

/// 结构化字段值类型(bool / 有符号 / 无符号 / 浮点 / 字符串)。
using FieldValue =
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

/// 单个结构化日志字段;name 必须为 snake_case 且不得命中敏感词黑名单。
struct Field {
    std::string name;
    FieldValue value;
    DataClass data_class{DataClass::Internal};
};

/// field() 辅助工厂:按值类型构造 Field,统一收窄为 FieldValue 支持的类型。
/// data_class 默认 Internal,敏感程度低的字段可显式标注。
[[nodiscard]] inline Field field(
    std::string name, bool value, DataClass data_class = DataClass::Internal) {
    return Field{std::move(name), value, data_class};
}

template <std::signed_integral Integer>
    requires(!std::same_as<Integer, bool>)
[[nodiscard]] inline Field field(
    std::string name,
    Integer value,
    DataClass data_class = DataClass::Internal) {
    return Field{std::move(name), static_cast<std::int64_t>(value), data_class};
}

template <std::unsigned_integral Integer>
    requires(!std::same_as<Integer, bool>)
[[nodiscard]] inline Field field(
    std::string name,
    Integer value,
    DataClass data_class = DataClass::Internal) {
    return Field{
        std::move(name), static_cast<std::uint64_t>(value), data_class};
}

template <std::floating_point Number>
[[nodiscard]] inline Field field(
    std::string name,
    Number value,
    DataClass data_class = DataClass::Internal) {
    return Field{std::move(name), static_cast<double>(value), data_class};
}

[[nodiscard]] inline Field field(
    std::string name,
    std::string value,
    DataClass data_class = DataClass::Internal) {
    return Field{std::move(name), std::move(value), data_class};
}

[[nodiscard]] inline Field field(
    std::string name,
    std::string_view value,
    DataClass data_class = DataClass::Internal) {
    return Field{std::move(name), std::string(value), data_class};
}

[[nodiscard]] inline Field field(
    std::string name,
    const char* value,
    DataClass data_class = DataClass::Internal) {
    return field(std::move(name), std::string_view(value), data_class);
}

/// 服务身份信息,随每条日志事件输出,用于多实例部署下的日志聚合与归属。
struct ServiceIdentity {
    std::string environment{
        "development"};            ///< 部署环境(如 development/production)
    std::string cluster{"local"};  ///< 集群名
    std::string region{"local"};   ///< 地理区域
    std::string service_name;      ///< 服务名(如 realm、gateway)
    std::string service_instance;  ///< 服务实例 ID(同服务多实例区分)
    std::string node_id;           ///< 节点 ID
    std::string zone;              ///< 可用区
};

/// 运行时可热更新的过滤配置(不涉及输出目标变更)。
struct LoggerRuntimeConfig {
    Severity min_severity{Severity::Info};  ///< 全局最低级别
    std::unordered_map<std::string, Severity>
        module_levels;  ///< 按模块覆盖级别
    std::unordered_map<std::string, double>
        sample_rates;  ///< 按事件名采样率 [0,1]
};

/// Logger 构造时的完整配置;构造后不可变,运行期调整请用 reconfigure()。
struct LoggerConfig {
    Severity min_severity{Severity::Info};  ///< 全局最低级别
    std::unordered_map<std::string, Severity>
        module_levels;  ///< 按模块覆盖级别
    std::unordered_map<std::string, double>
        sample_rates;                 ///< 按事件名采样率 [0,1]
    std::filesystem::path file_path;  ///< JSONL 输出文件路径(滚动)
    std::size_t normal_queue_capacity{8'192};  ///< 普通队列容量(Info 及以下)
    std::size_t priority_queue_capacity{2'048};  ///< 优先队列容量(Warn 及以上)
    std::size_t max_file_size{128U * 1024U * 1024U};  ///< 单个滚动文件大小上限
    std::size_t retained_files{8};            ///< 保留的滚动文件个数
    std::size_t max_event_size{16U * 1024U};  ///< 单条事件序列化后大小上限
    std::size_t max_string_size{4U * 1024U};  ///< 字符串字段截断长度
    bool console{false};                      ///< 是否同时输出到 stdout
};

/// 单条日志的可选关联上下文,用于链路追踪与请求聚合。
struct EventContext {
    std::optional<std::string> correlation_id;  ///< 关联 ID,须为 32 位小写 hex
    std::optional<std::uint64_t> request_id;  ///< 请求 ID
};

/// 日志提交结果,可用于调用方决定是否降级处理。
enum class LogResult : std::uint8_t {
    Accepted,  ///< 已通过校验并入队,等待异步写出
    Filtered,  ///< 被级别过滤或采样丢弃
    Dropped,   ///< 队列已满而被丢弃
    Rejected,  ///< 校验失败(非法名称、敏感字段、超限等)
};

/// Logger 运行统计,同时作为 Prometheus 指标的数据来源。
struct LogStats {
    std::uint64_t accepted{0};      ///< 成功入队总数
    std::uint64_t emitted{0};       ///< 实际写出总数
    std::uint64_t dropped{0};       ///< 队列满丢弃总数
    std::uint64_t rejected{0};      ///< 校验拒绝总数
    std::uint64_t truncated{0};     ///< 被截断事件总数
    std::uint64_t write_errors{0};  ///< 写出失败总数(降级到 stderr)
    std::uint64_t sampled_out{0};   ///< 被采样丢弃总数
    std::uint64_t normal_queue_dropped{0};    ///< 普通队列丢弃数
    std::uint64_t priority_queue_dropped{0};  ///< 优先队列丢弃数
    std::int64_t last_success_timestamp_seconds{
        0};  ///< 最近一次成功写出的 Unix 时间戳
    std::size_t normal_queue_size{0};        ///< 普通队列当前长度
    std::size_t priority_queue_size{0};      ///< 优先队列当前长度
    std::size_t normal_queue_capacity{0};    ///< 普通队列容量
    std::size_t priority_queue_capacity{0};  ///< 优先队列容量
};

/// 结构化 JSONL 日志服务。
///
/// 日志事件先经级别过滤 / 采样 / 名称与敏感字段校验,序列化为单行 JSON 后
/// 压入有界队列,由后台线程异步写入滚动文件(可选同时写 stdout)。
/// Warn 及以上事件走独立的优先队列,避免被普通日志洪峰淹没。
/// 写出失败时降级输出到 stderr 并计入 write_errors。
class Logger final {
public:
    /// 构造 Logger 并启动后台写出线程。
    /// file_path 为空或尺寸限制为 0 时抛出 std::invalid_argument。
    Logger(LoggerConfig config, ServiceIdentity identity);

    /// 析构时最多等待 2 秒冲刷剩余事件,再停止后台线程。
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) noexcept;
    Logger& operator=(Logger&&) noexcept;

    /// 提交一条结构化日志事件(线程安全)。
    /// event_name 须为 snake_case;字段名同样须为 snake_case 且不得为敏感词。
    /// location 默认捕获调用点,用于推导模块名与输出源码位置。
    [[nodiscard]] LogResult log(
        Severity severity,
        std::string_view event_name,
        std::string_view message = {},
        std::initializer_list<Field> fields = {},
        EventContext context = {},
        const std::source_location& location = std::source_location::current());

    /// 便捷接口:以 Info 级别提交,参数含义同 log()。
    [[nodiscard]] LogResult info(
        std::string_view event_name,
        std::string_view message = {},
        std::initializer_list<Field> fields = {},
        EventContext context = {},
        const std::source_location& location =
            std::source_location::current()) {
        return log(
            Severity::Info,
            event_name,
            message,
            fields,
            std::move(context),
            location);
    }

    [[nodiscard]] LogResult warn(
        std::string_view event_name,
        std::string_view message = {},
        std::initializer_list<Field> fields = {},
        EventContext context = {},
        const std::source_location& location =
            std::source_location::current()) {
        return log(
            Severity::Warn,
            event_name,
            message,
            fields,
            std::move(context),
            location);
    }

    [[nodiscard]] LogResult error(
        std::string_view event_name,
        std::string_view message = {},
        std::initializer_list<Field> fields = {},
        EventContext context = {},
        const std::source_location& location =
            std::source_location::current()) {
        return log(
            Severity::Error,
            event_name,
            message,
            fields,
            std::move(context),
            location);
    }

    /// 热更新全局最低级别,立即对后续日志生效。
    void set_min_severity(Severity severity) noexcept;

    /// 热更新过滤配置(级别、模块覆盖、采样率);采样率非法时抛出异常。
    void reconfigure(LoggerRuntimeConfig config);

    /// 等待队列中所有事件写出并冲刷底层 sink;超时返回 false。
    [[nodiscard]] bool flush(std::chrono::milliseconds timeout);

    /// 获取当前运行统计快照(线程安全)。
    [[nodiscard]] LogStats stats() const noexcept;

    /// 生成 Prometheus 文本格式的指标快照。
    [[nodiscard]] std::string prometheus_metrics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// Logger 指标 HTTP 服务配置。
struct MetricsServerConfig {
    std::string listen_address{"127.0.0.1"};  ///< 监听地址
    std::uint16_t port{0};  ///< 监听端口,0 表示随机分配
};

/// 在独立线程上暴露 GET /metrics 的轻量 HTTP 服务,
/// 返回 Logger 的 Prometheus 指标,供监控系统抓取。
class LoggerMetricsServer final {
public:
    /// 构造并启动服务;端口绑定失败时抛出 std::runtime_error。
    LoggerMetricsServer(Logger& logger, MetricsServerConfig config = {});
    /// 使用调用方提供的完整指标快照,供组合 Logger 与服务级指标。
    LoggerMetricsServer(
        std::function<std::string()> metrics_provider,
        MetricsServerConfig config = {});
    /// 停止 HTTP 服务并回收线程。
    ~LoggerMetricsServer();

    LoggerMetricsServer(const LoggerMetricsServer&) = delete;
    LoggerMetricsServer& operator=(const LoggerMetricsServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

/// 将 Severity 转为大写字符串(如 "INFO"、"WARN"),用于日志序列化输出。
[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

}  // namespace realm::observability
