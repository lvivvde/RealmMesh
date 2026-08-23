#include "realmmesh/observability/logger.hpp"

#include <gtest/gtest.h>
#include <httplib.h>
#include <nlohmann/json.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>

namespace realm::observability {
namespace {

class TemporaryLogFile final {
public:
    TemporaryLogFile()
        : path_(
              std::filesystem::temp_directory_path() /
              ("realmmesh-observability-" +
               std::to_string(std::chrono::steady_clock::now()
                                  .time_since_epoch()
                                  .count()) +
               ".jsonl")) {}

    ~TemporaryLogFile() {
        std::error_code error;
        std::filesystem::remove(path_, error);
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
};

TEST(LoggerTest, CallerCanWriteAStructuredDiagnosticEvent) {
    TemporaryLogFile file;
    LoggerConfig config;
    config.file_path = file.path();
    config.normal_queue_capacity = 8;
    config.priority_queue_capacity = 4;

    ServiceIdentity identity{
        .environment = "test",
        .cluster = "local",
        .region = "local",
        .service_name = "realm",
        .service_instance = "realm-test-1",
        .node_id = "node-test-1",
        .zone = "test-zone",
    };

    Logger logger(config, identity);
    EXPECT_EQ(
        logger.info(
            "player_session_established",
            "player session established",
            {field(
                 "account_id", std::uint64_t{10'001}, DataClass::Pseudonymous),
             field("request_id", std::uint64_t{42}),
             field("reconnected", false)}),
        LogResult::Accepted);
    ASSERT_TRUE(logger.flush(std::chrono::seconds(2)));

    std::ifstream input(file.path());
    ASSERT_TRUE(input.is_open());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
    const auto event = nlohmann::json::parse(line);

    EXPECT_EQ(event.at("schema_version"), 1);
    EXPECT_EQ(event.at("kind"), "diagnostic");
    EXPECT_EQ(event.at("severity"), "INFO");
    EXPECT_EQ(event.at("event_name"), "player_session_established");
    EXPECT_EQ(event.at("message"), "player session established");
    EXPECT_EQ(event.at("service_name"), "realm");
    EXPECT_EQ(event.at("service_instance"), "realm-test-1");
    EXPECT_EQ(event.at("attributes").at("account_id"), 10'001);
    EXPECT_EQ(
        event.at("attribute_data_classes").at("account_id"), "pseudonymous");
    EXPECT_EQ(event.at("attributes").at("request_id"), 42);
    EXPECT_EQ(event.at("attributes").at("reconnected"), false);
    EXPECT_TRUE(event.at("sequence").is_number_unsigned());
    EXPECT_FALSE(event.at("process_start_id").get<std::string>().empty());
    EXPECT_FALSE(event.at("timestamp").get<std::string>().empty());
}

TEST(LoggerTest, WorkerMakesAcceptedEventsVisibleWithoutExplicitFlush) {
    TemporaryLogFile file;
    LoggerConfig config;
    config.file_path = file.path();
    Logger logger(config, ServiceIdentity{.service_name = "login"});

    ASSERT_EQ(logger.info("service_started"), LogResult::Accepted);
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline &&
           (!std::filesystem::exists(file.path()) ||
            std::filesystem::file_size(file.path()) == 0)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    ASSERT_TRUE(std::filesystem::exists(file.path()));
    EXPECT_GT(std::filesystem::file_size(file.path()), 0U);
}

TEST(LoggerTest, WorkerDrainsAndFlushesBatchedEventsWithoutExplicitFlush) {
    TemporaryLogFile file;
    LoggerConfig config;
    config.file_path = file.path();
    config.normal_queue_capacity = 1024;
    config.priority_queue_capacity = 256;

    Logger logger(config, ServiceIdentity{.service_name = "batch"});
    constexpr int event_count = 200;
    for (int index = 0; index < event_count; ++index) {
        if (index % 10 == 0) {
            ASSERT_EQ(logger.warn("batch_warning_event"), LogResult::Accepted);
        } else {
            ASSERT_EQ(logger.info("batch_info_event"), LogResult::Accepted);
        }
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline &&
           logger.stats().emitted < static_cast<std::uint64_t>(event_count)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    const auto snapshot = logger.stats();
    EXPECT_EQ(snapshot.accepted, static_cast<std::uint64_t>(event_count));
    EXPECT_EQ(snapshot.emitted, static_cast<std::uint64_t>(event_count));
    EXPECT_EQ(snapshot.dropped, std::uint64_t{0});

    std::ifstream input(file.path());
    ASSERT_TRUE(input.is_open());
    int lines = 0;
    std::string line;
    while (static_cast<bool>(std::getline(input, line))) ++lines;
    EXPECT_EQ(lines, event_count);
}

TEST(LoggerTest, CallerCanAttachTrustedCorrelationContext) {
    TemporaryLogFile file;
    LoggerConfig config;
    config.file_path = file.path();
    ServiceIdentity identity{.service_name = "gateway"};
    Logger logger(config, identity);

    EXPECT_EQ(
        logger.info(
            "player_session_established",
            "player session established",
            {},
            EventContext{
                .correlation_id = "1234567890abcdef1234567890abcdef",
                .request_id = 42,
            }),
        LogResult::Accepted);
    ASSERT_TRUE(logger.flush(std::chrono::seconds(2)));

    std::ifstream input(file.path());
    std::string line;
    ASSERT_TRUE(static_cast<bool>(std::getline(input, line)));
    const auto event = nlohmann::json::parse(line);
    EXPECT_EQ(event.at("correlation_id"), "1234567890abcdef1234567890abcdef");
    EXPECT_EQ(event.at("request_id"), 42);
}

TEST(LoggerTest, MetricsExposeDeliveryAndValidationState) {
    TemporaryLogFile file;
    LoggerConfig config;
    config.file_path = file.path();
    config.max_string_size = 4;
    ServiceIdentity identity{
        .service_name = "login",
        .service_instance = "login-test-1",
    };
    Logger logger(config, identity);

    EXPECT_EQ(logger.info("accepted_event", "message"), LogResult::Accepted);
    EXPECT_EQ(
        logger.info(
            "rejected_event", {}, {field("token", "must-never-be-logged")}),
        LogResult::Rejected);
    ASSERT_TRUE(logger.flush(std::chrono::seconds(2)));

    const auto metrics = logger.prometheus_metrics();
    EXPECT_NE(
        metrics.find(
            "realmmesh_log_events_accepted_total{service_name=\"login\","
            "service_instance=\"login-test-1\"} 1"),
        std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_log_events_rejected_total"), std::string::npos);
    EXPECT_NE(metrics.find("} 1\n"), std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_log_events_truncated_total"),
        std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_log_normal_queue_size"), std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_log_priority_queue_size"), std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_log_normal_queue_capacity"), std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_log_priority_queue_capacity"),
        std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_log_last_success_timestamp_seconds"),
        std::string::npos);
}

TEST(LoggerTest, MetricsServerPublishesPrometheusEndpoint) {
    TemporaryLogFile file;
    LoggerConfig config;
    config.file_path = file.path();
    Logger logger(
        config,
        ServiceIdentity{
            .service_name = "realm",
            .service_instance = "realm-test-1",
        });
    LoggerMetricsServer metrics(
        logger, MetricsServerConfig{.listen_address = "127.0.0.1", .port = 0});

    ASSERT_GT(metrics.port(), 0);
    httplib::Client client("127.0.0.1", metrics.port());
    const auto response = client.Get("/metrics");
    ASSERT_TRUE(response);
    EXPECT_EQ(response->status, 200);
    EXPECT_EQ(
        response->get_header_value("Content-Type").substr(0, 10), "text/plain");
    EXPECT_NE(
        response->body.find("realmmesh_log_events_accepted_total"),
        std::string::npos);
}

TEST(LoggerTest, RuntimePolicyFiltersModulesAndSamplesOnlyLowPriorityEvents) {
    TemporaryLogFile file;
    LoggerConfig config;
    config.file_path = file.path();
    config.min_severity = Severity::Trace;
    config.module_levels.emplace("tests.cpp", Severity::Error);
    Logger logger(config, ServiceIdentity{.service_name = "gateway"});

    EXPECT_EQ(logger.info("module_filtered"), LogResult::Filtered);

    LoggerRuntimeConfig runtime;
    runtime.min_severity = Severity::Trace;
    runtime.sample_rates.emplace("sampled_event", 0.0);
    logger.reconfigure(std::move(runtime));

    EXPECT_EQ(logger.info("sampled_event"), LogResult::Filtered);
    EXPECT_EQ(logger.warn("sampled_event"), LogResult::Accepted);
    EXPECT_EQ(logger.info("retained_event"), LogResult::Accepted);
    ASSERT_TRUE(logger.flush(std::chrono::seconds(2)));

    const auto stats = logger.stats();
    EXPECT_EQ(stats.sampled_out, 1U);
    EXPECT_EQ(stats.accepted, 2U);
}

TEST(LoggerTest, DevelopmentCanFallBackToStderrButProductionFailsClosed) {
    LoggerConfig config;
    config.file_path = "/proc/realmmesh-observability/unwritable.jsonl";

    EXPECT_NO_THROW({
        Logger logger(
            config,
            ServiceIdentity{
                .environment = "development",
                .service_name = "login",
            });
        EXPECT_EQ(logger.stats().write_errors, 1U);
    });

    EXPECT_THROW(
        Logger(
            config,
            ServiceIdentity{
                .environment = "production",
                .service_name = "login",
            }),
        std::filesystem::filesystem_error);
}

}  // namespace
}  // namespace realm::observability
