#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
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

enum class Severity : std::uint8_t {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
    Fatal,
};

enum class DataClass : std::uint8_t {
    Public,
    Internal,
    Pseudonymous,
};

using FieldValue =
    std::variant<bool, std::int64_t, std::uint64_t, double, std::string>;

struct Field {
    std::string name;
    FieldValue value;
    DataClass data_class{DataClass::Internal};
};

[[nodiscard]] inline Field field(
    std::string name,
    bool value,
    DataClass data_class = DataClass::Internal) {
    return Field{std::move(name), value, data_class};
}

template <std::signed_integral Integer>
    requires(!std::same_as<Integer, bool>)
[[nodiscard]] inline Field field(
    std::string name,
    Integer value,
    DataClass data_class = DataClass::Internal) {
    return Field{
        std::move(name), static_cast<std::int64_t>(value), data_class};
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
    return Field{
        std::move(name), std::string(value), data_class};
}

[[nodiscard]] inline Field field(
    std::string name,
    const char* value,
    DataClass data_class = DataClass::Internal) {
    return field(std::move(name), std::string_view(value), data_class);
}

struct ServiceIdentity {
    std::string environment{"development"};
    std::string cluster{"local"};
    std::string region{"local"};
    std::string service_name;
    std::string service_instance;
    std::string node_id;
    std::string zone;
};

struct LoggerRuntimeConfig {
    Severity min_severity{Severity::Info};
    std::unordered_map<std::string, Severity> module_levels;
    std::unordered_map<std::string, double> sample_rates;
};

struct LoggerConfig {
    Severity min_severity{Severity::Info};
    std::unordered_map<std::string, Severity> module_levels;
    std::unordered_map<std::string, double> sample_rates;
    std::filesystem::path file_path;
    std::size_t normal_queue_capacity{8'192};
    std::size_t priority_queue_capacity{2'048};
    std::size_t max_file_size{128U * 1024U * 1024U};
    std::size_t retained_files{8};
    std::size_t max_event_size{16U * 1024U};
    std::size_t max_string_size{4U * 1024U};
    bool console{false};
};

struct EventContext {
    std::optional<std::string> correlation_id;
    std::optional<std::uint64_t> request_id;
};

enum class LogResult : std::uint8_t {
    Accepted,
    Filtered,
    Dropped,
    Rejected,
};

struct LogStats {
    std::uint64_t accepted{0};
    std::uint64_t emitted{0};
    std::uint64_t dropped{0};
    std::uint64_t rejected{0};
    std::uint64_t truncated{0};
    std::uint64_t write_errors{0};
    std::uint64_t sampled_out{0};
    std::uint64_t normal_queue_dropped{0};
    std::uint64_t priority_queue_dropped{0};
    std::int64_t last_success_timestamp_seconds{0};
    std::size_t normal_queue_size{0};
    std::size_t priority_queue_size{0};
    std::size_t normal_queue_capacity{0};
    std::size_t priority_queue_capacity{0};
};

class Logger final {
public:
    Logger(LoggerConfig config, ServiceIdentity identity);
    ~Logger();

    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    Logger(Logger&&) noexcept;
    Logger& operator=(Logger&&) noexcept;

    [[nodiscard]] LogResult log(
        Severity severity,
        std::string_view event_name,
        std::string_view message = {},
        std::initializer_list<Field> fields = {},
        EventContext context = {},
        const std::source_location& location =
            std::source_location::current());

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

    void set_min_severity(Severity severity) noexcept;
    void reconfigure(LoggerRuntimeConfig config);
    [[nodiscard]] bool flush(std::chrono::milliseconds timeout);
    [[nodiscard]] LogStats stats() const noexcept;
    [[nodiscard]] std::string prometheus_metrics() const;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

struct MetricsServerConfig {
    std::string listen_address{"127.0.0.1"};
    std::uint16_t port{0};
};

class LoggerMetricsServer final {
public:
    LoggerMetricsServer(Logger& logger, MetricsServerConfig config = {});
    ~LoggerMetricsServer();

    LoggerMetricsServer(const LoggerMetricsServer&) = delete;
    LoggerMetricsServer& operator=(const LoggerMetricsServer&) = delete;

    [[nodiscard]] std::uint16_t port() const noexcept;

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view to_string(Severity severity) noexcept;

}  // namespace realm::observability
