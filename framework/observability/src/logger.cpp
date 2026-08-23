#include "realmmesh/observability/logger.hpp"

#include "realmmesh/concurrency/bounded_queue.hpp"

#include <httplib.h>
#include <spdlog/logger.h>
#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_sinks.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <ctime>
#include <exception>
#include <functional>
#include <iomanip>
#include <limits>
#include <mutex>
#include <random>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace realm::observability {
namespace {

/// 单条日志事件允许的最大属性字段数,超出直接拒绝。
constexpr std::size_t max_attribute_count = 64;

/// 取严重级别的序数值,数值越大越严重,用于级别比较。
[[nodiscard]] int severity_rank(Severity severity) noexcept {
    return static_cast<int>(severity);
}

/// 生成纳秒精度的 UTC 时间戳,格式为 ISO 8601:2026-08-23T12:34:56.123456789Z
[[nodiscard]] std::string utc_timestamp() {
    const auto now = std::chrono::system_clock::now();
    const auto seconds = std::chrono::floor<std::chrono::seconds>(now);
    const auto nanoseconds =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds);
    const std::time_t time = std::chrono::system_clock::to_time_t(seconds);
    std::tm value{};
#if defined(_WIN32)
    gmtime_s(&value, &time);
#else
    gmtime_r(&time, &value);
#endif
    std::array<char, 32> buffer{};
    static_cast<void>(std::strftime(
        buffer.data(), buffer.size(), "%Y-%m-%dT%H:%M:%S", &value));
    std::ostringstream output;
    output << buffer.data() << '.' << std::setfill('0') << std::setw(9)
           << nanoseconds.count() << 'Z';
    return output.str();
}

/// 生成 16 字节随机数的 32 位小写 hex 串,用作进程启动实例 ID。
[[nodiscard]] std::string random_hex_id() {
    std::array<unsigned char, 16> bytes{};
    std::random_device random;
    for (auto& byte : bytes) {
        byte = static_cast<unsigned char>(random());
    }
    constexpr std::string_view digits = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        result.push_back(digits[(byte >> 4U) & 0x0fU]);
        result.push_back(digits[byte & 0x0fU]);
    }
    return result;
}

/// 校验是否为合法 snake_case:小写字母/数字/单个下划线,不以 _ 开头或结尾,
/// 长度 1~64。event_name 与字段名都必须满足此约束。
[[nodiscard]] bool is_snake_case(std::string_view value) noexcept {
    if (value.empty() || value.size() > 64 || value.front() == '_' ||
        value.back() == '_') {
        return false;
    }
    bool previous_underscore = false;
    for (const char character : value) {
        const bool underscore = character == '_';
        const bool valid = underscore ||
                           (character >= 'a' && character <= 'z') ||
                           (character >= '0' && character <= '9');
        if (!valid || (underscore && previous_underscore)) return false;
        previous_underscore = underscore;
    }
    return true;
}

/// 敏感字段名黑名单:命中即拒绝整条日志,防止凭据类信息落盘。
[[nodiscard]] bool is_forbidden_field(std::string_view name) {
    static const std::unordered_set<std::string_view> forbidden{
        "authorization",
        "credential",
        "password",
        "private_key",
        "secret",
        "ticket",
        "token",
    };
    return forbidden.contains(name);
}

/// 校验关联 ID 格式:恰好 32 位小写 hex(与 random_hex_id() 输出一致)。
[[nodiscard]] bool is_correlation_id(std::string_view value) noexcept {
    return value.size() == 32 && std::ranges::all_of(value, [](char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

/// DataClass 枚举转小写字符串,写入 attribute_data_classes。
[[nodiscard]] std::string_view to_string(DataClass data_class) noexcept {
    switch (data_class) {
    case DataClass::Public:
        return "public";
    case DataClass::Internal:
        return "internal";
    case DataClass::Pseudonymous:
        return "pseudonymous";
    }
    return "unknown";
}

/// 从源码文件路径推导模块名:定位 /tests/ /apps/ /framework/ /game/ 标记,
/// 取标记后两级目录并以 '.' 连接,如 "framework.observability"。
/// 匹配失败返回 "unknown"。
[[nodiscard]] std::string module_from_source(std::string_view source) {
    constexpr std::array markers{"/tests/", "/apps/", "/framework/", "/game/"};
    for (const std::string_view marker : markers) {
        const auto marker_position = source.rfind(marker);
        if (marker_position == std::string_view::npos) continue;
        const auto begin = marker_position + 1;
        const auto first_slash = source.find('/', begin);
        if (first_slash == std::string_view::npos) break;
        const auto second_slash = source.find('/', first_slash + 1);
        const auto end = second_slash == std::string_view::npos ? source.size()
                                                                : second_slash;
        std::string module(source.substr(begin, end - begin));
        std::ranges::replace(module, '/', '.');
        return module;
    }
    return "unknown";
}

/// 确定性采样判定:用事件名哈希异或序列号做伪随机,归一化后与采样率比较。
/// rate <= 0 全部丢弃,rate >= 1 全部保留;同一事件按序列近似均匀采样。
[[nodiscard]] bool retain_sample(
    std::string_view event_name, double rate, std::uint64_t sequence) noexcept {
    if (rate <= 0.0) return false;
    if (rate >= 1.0) return true;
    const auto mixed =
        static_cast<std::uint64_t>(std::hash<std::string_view>{}(event_name)) ^
        (sequence * 0x9e3779b97f4a7c15ULL);
    const auto normalized =
        static_cast<double>(mixed) /
        static_cast<double>(std::numeric_limits<std::uint64_t>::max());
    return normalized < rate;
}

/// 获取当前进程 ID,写入日志事件的 process_id 字段。
[[nodiscard]] std::uint64_t process_id() noexcept {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

/// 转义 Prometheus 标签值中的 \ " 与换行,保证指标输出格式合法。
[[nodiscard]] std::string prometheus_label(std::string_view value) {
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

/// 将 Field 写入 JSON attributes;超长字符串截断到 max_string_size 并置
/// truncated 标记。
void set_json_value(
    nlohmann::json& attributes,
    const Field& field_value,
    std::size_t max_string_size,
    bool& truncated) {
    std::visit(
        [&](const auto& value) {
            using Value = std::decay_t<decltype(value)>;
            if constexpr (std::same_as<Value, std::string>) {
                if (value.size() > max_string_size) {
                    attributes[field_value.name] =
                        value.substr(0, max_string_size);
                    truncated = true;
                } else {
                    attributes[field_value.name] = value;
                }
            } else {
                attributes[field_value.name] = value;
            }
        },
        field_value.value);
}

}  // namespace

/// Logger 的具体实现(Pimpl):持有双队列、spdlog sink 与后台写出线程。
class Logger::Impl final {
public:
    /// 构造:校验配置 → 创建滚动文件 sink(可选 stdout)→ 启动两条消费线程。
    /// development 环境下文件创建失败降级为 stderr sink,其他环境直接抛出。
    Impl(LoggerConfig config, ServiceIdentity identity)
        : config_(std::move(config)),
          identity_(std::move(identity)),
          normal_queue_(config_.normal_queue_capacity),
          priority_queue_(config_.priority_queue_capacity),
          runtime_config_(
              std::make_shared<const LoggerRuntimeConfig>(LoggerRuntimeConfig{
                  .min_severity = config_.min_severity,
                  .module_levels = config_.module_levels,
                  .sample_rates = config_.sample_rates,
              })),
          process_start_id_(random_hex_id()) {
        if (config_.file_path.empty()) {
            throw std::invalid_argument("logger file path cannot be empty");
        }
        if (config_.max_file_size == 0 || config_.retained_files == 0 ||
            config_.max_event_size == 0 || config_.max_string_size == 0) {
            throw std::invalid_argument("logger size limits must be positive");
        }
        for (const auto& [event_name, rate] : config_.sample_rates) {
            if (!is_snake_case(event_name) || rate < 0.0 || rate > 1.0) {
                throw std::invalid_argument(
                    "logger sample rates require snake_case names and values in [0, 1]");
            }
        }
        std::vector<spdlog::sink_ptr> sinks;
        bool stderr_fallback = false;
        try {
            const auto parent = config_.file_path.parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            sinks.push_back(
                std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
                    config_.file_path.string(),
                    config_.max_file_size,
                    config_.retained_files));
            if (config_.console) {
                sinks.push_back(
                    std::make_shared<spdlog::sinks::stdout_sink_mt>());
            }
        } catch (const std::exception&) {
            if (identity_.environment != "development") throw;
            sinks.clear();
            sinks.push_back(std::make_shared<spdlog::sinks::stderr_sink_mt>());
            stderr_fallback = true;
        }
        sink_ = std::make_shared<spdlog::logger>(
            "realmmesh-structured-" + process_start_id_,
            sinks.begin(),
            sinks.end());
        sink_->set_pattern("%v");
        if (stderr_fallback) {
            write_errors_.store(1, std::memory_order_relaxed);
        }

        normal_worker_ = std::jthread([this](std::stop_token stop) {
            consume(normal_queue_, stop);
        });
        priority_worker_ = std::jthread([this](std::stop_token stop) {
            consume(priority_queue_, stop);
        });
    }

    /// 析构:先冲刷(最多 2 秒),再请求停止并唤醒后台线程。
    ~Impl() {
        static_cast<void>(flush(std::chrono::seconds(2)));
        normal_worker_.request_stop();
        priority_worker_.request_stop();
        work_ready_.notify_all();
    }

    /// 日志提交主流程(调用方线程执行,线程安全):
    /// 1. 按模块/全局级别过滤;
    /// 2. 低于 Warn 的事件按事件名采样;
    /// 3. 校验 event_name / 字段名 / correlation_id;
    /// 4. 组装 JSON 并按 max_event_size 逐级裁剪(attributes → message);
    /// 5. Warn 及以上入优先队列,其余入普通队列;队列满则丢弃并计数。
    [[nodiscard]] LogResult log(
        Severity severity,
        std::string_view event_name,
        std::string_view message,
        std::initializer_list<Field> fields,
        EventContext context,
        const std::source_location& location) {
        const auto runtime = runtime_config_.load(std::memory_order_acquire);
        const auto module = module_from_source(location.file_name());
        const auto module_level = runtime->module_levels.find(module);
        const auto minimum = module_level == runtime->module_levels.end()
                                 ? runtime->min_severity
                                 : module_level->second;
        if (severity_rank(severity) < severity_rank(minimum)) {
            return LogResult::Filtered;
        }
        if (severity_rank(severity) < severity_rank(Severity::Warn)) {
            const auto sample =
                runtime->sample_rates.find(std::string(event_name));
            if (sample != runtime->sample_rates.end() &&
                !retain_sample(
                    event_name,
                    sample->second,
                    sample_sequence_.fetch_add(1, std::memory_order_relaxed) +
                        1)) {
                sampled_out_.fetch_add(1, std::memory_order_relaxed);
                return LogResult::Filtered;
            }
        }
        if (!is_snake_case(event_name) || fields.size() > max_attribute_count ||
            (context.correlation_id.has_value() &&
             !is_correlation_id(*context.correlation_id))) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return LogResult::Rejected;
        }
        for (const auto& item : fields) {
            if (!is_snake_case(item.name) || is_forbidden_field(item.name)) {
                rejected_.fetch_add(1, std::memory_order_relaxed);
                return LogResult::Rejected;
            }
        }

        bool event_truncated = false;
        std::string event_message(message);
        if (event_message.size() > config_.max_string_size) {
            event_message.resize(config_.max_string_size);
            event_truncated = true;
        }

        nlohmann::json event{
            {"schema_version", 1},
            {"timestamp", utc_timestamp()},
            {"severity", to_string(severity)},
            {"kind", "diagnostic"},
            {"event_name", event_name},
            {"module", module},
            {"message", std::move(event_message)},
            {"environment", identity_.environment},
            {"cluster", identity_.cluster},
            {"region", identity_.region},
            {"service_name", identity_.service_name},
            {"service_instance", identity_.service_instance},
            {"node_id", identity_.node_id},
            {"zone", identity_.zone},
            {"process_start_id", process_start_id_},
            {"process_id", process_id()},
            {"thread_id",
             std::hash<std::thread::id>{}(std::this_thread::get_id())},
            {"sequence", sequence_.fetch_add(1, std::memory_order_relaxed) + 1},
            {"source_file", location.file_name()},
            {"source_line", location.line()},
            {"attributes", nlohmann::json::object()},
            {"attribute_data_classes", nlohmann::json::object()},
        };
        for (const auto& item : fields) {
            set_json_value(
                event["attributes"],
                item,
                config_.max_string_size,
                event_truncated);
            event["attribute_data_classes"][item.name] =
                to_string(item.data_class);
        }
        if (context.correlation_id.has_value()) {
            event["correlation_id"] = std::move(*context.correlation_id);
        }
        if (context.request_id.has_value()) {
            event["request_id"] = *context.request_id;
        }

        std::string encoded = event.dump();
        while (encoded.size() > config_.max_event_size &&
               !event["attributes"].empty()) {
            event["attributes"].erase(std::prev(event["attributes"].end()));
            event["attribute_data_classes"].erase(
                std::prev(event["attribute_data_classes"].end()));
            event_truncated = true;
            encoded = event.dump();
        }
        if (encoded.size() > config_.max_event_size) {
            event["message"] = "";
            event_truncated = true;
        }
        if (event_truncated) {
            event["truncated"] = true;
            truncated_.fetch_add(1, std::memory_order_relaxed);
        }
        encoded = event.dump();
        if (encoded.size() > config_.max_event_size) {
            rejected_.fetch_add(1, std::memory_order_relaxed);
            return LogResult::Rejected;
        }

        pending_.fetch_add(1, std::memory_order_acq_rel);
        const bool high_priority =
            severity_rank(severity) >= severity_rank(Severity::Warn);
        auto& queue = high_priority ? priority_queue_ : normal_queue_;
        if (!queue.try_push(std::move(encoded))) {
            pending_.fetch_sub(1, std::memory_order_acq_rel);
            dropped_.fetch_add(1, std::memory_order_relaxed);
            if (high_priority) {
                priority_queue_dropped_.fetch_add(1, std::memory_order_relaxed);
            } else {
                normal_queue_dropped_.fetch_add(1, std::memory_order_relaxed);
            }
            flushed_.notify_all();
            return LogResult::Dropped;
        }
        accepted_.fetch_add(1, std::memory_order_relaxed);
        work_ready_.notify_all();
        return LogResult::Accepted;
    }

    /// 运行时配置通过 shared_ptr 原子替换,实现无锁热更新。
    void set_min_severity(Severity severity) noexcept {
        auto updated = *runtime_config_.load(std::memory_order_acquire);
        updated.min_severity = severity;
        runtime_config_.store(
            std::make_shared<const LoggerRuntimeConfig>(std::move(updated)),
            std::memory_order_release);
    }

    /// 整体替换运行时过滤配置;采样率名称或取值非法时抛 std::invalid_argument。
    void reconfigure(LoggerRuntimeConfig config) {
        for (const auto& [event_name, rate] : config.sample_rates) {
            if (!is_snake_case(event_name) || rate < 0.0 || rate > 1.0) {
                throw std::invalid_argument(
                    "logger sample rates require snake_case names and values in [0, 1]");
            }
        }
        runtime_config_.store(
            std::make_shared<const LoggerRuntimeConfig>(std::move(config)),
            std::memory_order_release);
    }

    /// 等待 pending_ 归零(所有入队事件已写出)后冲刷底层 sink;超时返回 false。
    [[nodiscard]] bool flush(std::chrono::milliseconds timeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::unique_lock lock(flush_mutex_);
        if (!flushed_.wait_until(lock, deadline, [this] {
                return pending_.load(std::memory_order_acquire) == 0;
            })) {
            return false;
        }
        try {
            sink_->flush();
            return true;
        } catch (const std::exception&) {
            write_errors_.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }

    /// 汇总各原子计数与队列长度,生成统计快照。
    [[nodiscard]] LogStats stats() const noexcept {
        return LogStats{
            .accepted = accepted_.load(std::memory_order_relaxed),
            .emitted = emitted_.load(std::memory_order_relaxed),
            .dropped = dropped_.load(std::memory_order_relaxed),
            .rejected = rejected_.load(std::memory_order_relaxed),
            .truncated = truncated_.load(std::memory_order_relaxed),
            .write_errors = write_errors_.load(std::memory_order_relaxed),
            .sampled_out = sampled_out_.load(std::memory_order_relaxed),
            .normal_queue_dropped =
                normal_queue_dropped_.load(std::memory_order_relaxed),
            .priority_queue_dropped =
                priority_queue_dropped_.load(std::memory_order_relaxed),
            .last_success_timestamp_seconds =
                last_success_timestamp_seconds_.load(std::memory_order_relaxed),
            .normal_queue_size = normal_queue_.size(),
            .priority_queue_size = priority_queue_.size(),
            .normal_queue_capacity = normal_queue_.capacity(),
            .priority_queue_capacity = priority_queue_.capacity(),
        };
    }

    /// 输出 Prometheus 文本格式指标:计数器(accepted/emitted/dropped/…)、
    /// 按队列维度的丢弃数、队列水位 gauge 与最近成功写出时间戳。
    [[nodiscard]] std::string prometheus_metrics() const {
        const auto snapshot = stats();
        const std::string labels =
            "{service_name=\"" + prometheus_label(identity_.service_name) +
            "\",service_instance=\"" +
            prometheus_label(identity_.service_instance) + "\"}";
        std::ostringstream output;
        const auto counter = [&](std::string_view name, std::uint64_t value) {
            output << "# TYPE " << name << " counter\n"
                   << name << labels << ' ' << value << '\n';
        };
        const auto gauge = [&](std::string_view name, std::size_t value) {
            output << "# TYPE " << name << " gauge\n"
                   << name << labels << ' ' << value << '\n';
        };
        counter("realmmesh_log_events_accepted_total", snapshot.accepted);
        counter("realmmesh_log_events_emitted_total", snapshot.emitted);
        counter("realmmesh_log_events_dropped_total", snapshot.dropped);
        counter("realmmesh_log_events_rejected_total", snapshot.rejected);
        counter("realmmesh_log_events_truncated_total", snapshot.truncated);
        counter("realmmesh_log_write_errors_total", snapshot.write_errors);
        counter("realmmesh_log_events_sampled_out_total", snapshot.sampled_out);
        const auto queue_labels = [&](std::string_view queue) {
            return labels.substr(0, labels.size() - 1) + ",queue=\"" +
                   std::string(queue) + "\",reason=\"full\"}";
        };
        output << "# TYPE realmmesh_log_queue_dropped_total counter\n"
               << "realmmesh_log_queue_dropped_total" << queue_labels("normal")
               << ' ' << snapshot.normal_queue_dropped << '\n'
               << "realmmesh_log_queue_dropped_total"
               << queue_labels("priority") << ' '
               << snapshot.priority_queue_dropped << '\n';
        gauge("realmmesh_log_normal_queue_size", snapshot.normal_queue_size);
        gauge(
            "realmmesh_log_priority_queue_size", snapshot.priority_queue_size);
        gauge(
            "realmmesh_log_normal_queue_capacity",
            snapshot.normal_queue_capacity);
        gauge(
            "realmmesh_log_priority_queue_capacity",
            snapshot.priority_queue_capacity);
        output << "# TYPE realmmesh_log_last_success_timestamp_seconds gauge\n"
               << "realmmesh_log_last_success_timestamp_seconds" << labels
               << ' ' << snapshot.last_success_timestamp_seconds << '\n';
        return output.str();
    }

private:
    /// 后台消费循环:弹出事件 → 写 sink 并逐条 flush → 失败降级 stderr;
    /// 队列空时在条件变量上等待,最长 20ms 醒来检查停止标志。
    void consume(
        concurrency::BoundedQueue<std::string>& queue, std::stop_token stop) {
        while (true) {
            if (auto event = queue.try_pop(); event.has_value()) {
                try {
                    sink_->info(*event);
                    sink_->flush();
                    emitted_.fetch_add(1, std::memory_order_relaxed);
                    last_success_timestamp_seconds_.store(
                        std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::system_clock::now().time_since_epoch())
                            .count(),
                        std::memory_order_relaxed);
                } catch (const std::exception&) {
                    write_errors_.fetch_add(1, std::memory_order_relaxed);
                    std::fputs(event->c_str(), stderr);
                    std::fputc('\n', stderr);
                }
                pending_.fetch_sub(1, std::memory_order_acq_rel);
                flushed_.notify_all();
                continue;
            }
            if (stop.stop_requested()) return;
            std::unique_lock lock(work_mutex_);
            work_ready_.wait_for(lock, std::chrono::milliseconds(20), [&] {
                return stop.stop_requested() || queue.size() != 0;
            });
        }
    }

    LoggerConfig config_;
    ServiceIdentity identity_;
    concurrency::BoundedQueue<std::string> normal_queue_;
    concurrency::BoundedQueue<std::string> priority_queue_;
    std::shared_ptr<spdlog::logger> sink_;
    std::atomic<std::shared_ptr<const LoggerRuntimeConfig>> runtime_config_;
    const std::string process_start_id_;
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<std::uint64_t> sample_sequence_{0};
    std::atomic<std::uint64_t> pending_{0};
    std::atomic<std::uint64_t> accepted_{0};
    std::atomic<std::uint64_t> emitted_{0};
    std::atomic<std::uint64_t> dropped_{0};
    std::atomic<std::uint64_t> rejected_{0};
    std::atomic<std::uint64_t> truncated_{0};
    std::atomic<std::uint64_t> write_errors_{0};
    std::atomic<std::uint64_t> sampled_out_{0};
    std::atomic<std::uint64_t> normal_queue_dropped_{0};
    std::atomic<std::uint64_t> priority_queue_dropped_{0};
    std::atomic<std::int64_t> last_success_timestamp_seconds_{0};
    std::mutex work_mutex_;
    std::condition_variable work_ready_;
    std::mutex flush_mutex_;
    std::condition_variable flushed_;
    std::jthread normal_worker_;
    std::jthread priority_worker_;
};

/// LoggerMetricsServer 的具体实现:注册 /metrics 路由,绑定端口并启动监听线程。
class LoggerMetricsServer::Impl final {
public:
    /// port 为 0 时随机分配可用端口;绑定失败抛 std::runtime_error。
    Impl(Logger& logger, MetricsServerConfig config)
        : logger_(logger) {
        server_.Get(
            "/metrics",
            [this](const httplib::Request&, httplib::Response& response) {
                response.set_content(
                    logger_.prometheus_metrics(),
                    "text/plain; version=0.0.4; charset=utf-8");
            });
        const int bound_port =
            config.port == 0
                ? server_.bind_to_any_port(config.listen_address)
                : (server_.bind_to_port(config.listen_address, config.port)
                       ? static_cast<int>(config.port)
                       : -1);
        if (bound_port <= 0 || bound_port > 65'535) {
            throw std::runtime_error(
                "failed to bind logger metrics endpoint at " +
                config.listen_address + ':' + std::to_string(config.port));
        }
        port_ = static_cast<std::uint16_t>(bound_port);
        thread_ = std::jthread([this] {
            static_cast<void>(server_.listen_after_bind());
        });
        server_.wait_until_ready();
    }

    ~Impl() {
        server_.stop();
        if (thread_.joinable()) thread_.join();
    }

    [[nodiscard]] std::uint16_t port() const noexcept { return port_; }

private:
    Logger& logger_;
    httplib::Server server_;
    std::jthread thread_;
    std::uint16_t port_{0};
};

Logger::Logger(LoggerConfig config, ServiceIdentity identity)
    : impl_(std::make_unique<Impl>(std::move(config), std::move(identity))) {}

Logger::~Logger() = default;
Logger::Logger(Logger&&) noexcept = default;
Logger& Logger::operator=(Logger&&) noexcept = default;

LogResult Logger::log(
    Severity severity,
    std::string_view event_name,
    std::string_view message,
    std::initializer_list<Field> fields,
    EventContext context,
    const std::source_location& location) {
    return impl_->log(
        severity, event_name, message, fields, std::move(context), location);
}

void Logger::set_min_severity(Severity severity) noexcept {
    impl_->set_min_severity(severity);
}

void Logger::reconfigure(LoggerRuntimeConfig config) {
    impl_->reconfigure(std::move(config));
}

bool Logger::flush(std::chrono::milliseconds timeout) {
    return impl_->flush(timeout);
}

LogStats Logger::stats() const noexcept { return impl_->stats(); }

std::string Logger::prometheus_metrics() const {
    return impl_->prometheus_metrics();
}

LoggerMetricsServer::LoggerMetricsServer(
    Logger& logger, MetricsServerConfig config)
    : impl_(std::make_unique<Impl>(logger, std::move(config))) {}

LoggerMetricsServer::~LoggerMetricsServer() = default;

std::uint16_t LoggerMetricsServer::port() const noexcept {
    return impl_->port();
}

std::string_view to_string(Severity severity) noexcept {
    switch (severity) {
    case Severity::Trace:
        return "TRACE";
    case Severity::Debug:
        return "DEBUG";
    case Severity::Info:
        return "INFO";
    case Severity::Warn:
        return "WARN";
    case Severity::Error:
        return "ERROR";
    case Severity::Fatal:
        return "FATAL";
    }
    return "UNKNOWN";
}

}  // namespace realm::observability
