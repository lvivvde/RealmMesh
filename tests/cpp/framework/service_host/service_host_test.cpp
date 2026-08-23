#include "realmmesh/service_host/service_host.hpp"

#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>

namespace realm::service_host {
namespace {

/// TLS 证书/会话票据环境变量守护:指向 CMake 预生成的自签证书与固定
/// 测试密钥,析构时还原。
class ScopedTlsEnvironment final {
public:
    ScopedTlsEnvironment() {
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_TLS_CERTIFICATE_FILE",
                REALMMESH_TEST_TLS_CERTIFICATE,
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_TLS_PRIVATE_KEY_FILE",
                REALMMESH_TEST_TLS_PRIVATE_KEY,
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_SESSION_TICKET_KEY",
                "0102030405060708090a0b0c0d0e0f10"
                "1112131415161718191a1b1c1d1e1f20",
                1),
            0);
    }
    ~ScopedTlsEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
    }
};

class ServiceHostTest : public ::testing::Test {
protected:
    std::filesystem::path root_;

    void SetUp() override {
        root_ =
            std::filesystem::temp_directory_path() /
            ("service-host-" +
             std::to_string(::testing::UnitTest::GetInstance()->random_seed()));
        std::filesystem::create_directories(root_ / "common");
        std::filesystem::create_directories(root_ / "services");
        write(
            root_ / "common" / "discovery.lua",
            "return { service_discovery = { enabled = false, instance_id = \"host-test-01\", endpoint = \"http://etcd:2379\", lease_ttl_seconds = 9 } }");
        write(
            root_ / "services" / "host_test.lua",
            "return { " + transport_lua() + " }");
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::remove_all(root_, error);
    }

    /// 单个 tls_tcp transport(listen_port = 0 随机,证书走环境变量)。
    static std::string transport_lua() {
        return "transports = { { name = \"client_tls_tcp\", protocol = "
               "\"tls_tcp\", enabled = true, listen_address = "
               "\"127.0.0.1\", listen_port = 0, "
               "certificate_chain_file_environment = "
               "\"REALMMESH_TLS_CERTIFICATE_FILE\", "
               "private_key_file_environment = "
               "\"REALMMESH_TLS_PRIVATE_KEY_FILE\" } }";
    }

    static void write(
        const std::filesystem::path& path, std::string_view text) {
        std::ofstream output(path);
        output << text;
    }
};

TEST_F(ServiceHostTest, StartsRuntimeAndReadiesWithoutDiscovery) {
    const ScopedTlsEnvironment tls_environment;
    ServiceHost host(root_, "host_test");

    // 启动前 ready gauge 为 0。
    EXPECT_NE(
        host.prometheus_metrics().find(
            "realmmesh_service_ready{service_name=\"host_test\","
            "service_instance=\"host-test-01\"} 0"),
        std::string::npos);

    EXPECT_TRUE(host.start());
    EXPECT_TRUE(host.ready());
    EXPECT_NE(host.runtime().local_port(), std::uint16_t{0});

    // 指标为 logger 文本 + realmmesh_service_ready gauge 拼接。
    const auto metrics = host.prometheus_metrics();
    EXPECT_NE(
        metrics.find("realmmesh_log_events_accepted_total"), std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_service_ready{service_name=\"host_test\","
                     "service_instance=\"host-test-01\"} 1"),
        std::string::npos);

    host.stop();
    EXPECT_FALSE(host.runtime().running());
    host.stop();  // 幂等:重复关停无害。
}

TEST_F(ServiceHostTest, UnknownServiceNameWithDiscoveryThrows) {
    const ScopedTlsEnvironment tls_environment;
    write(
        root_ / "services" / "mystery.lua",
        "return { " + transport_lua() +
            ", service_discovery = { enabled = true, instance_id = "
            "\"mystery-01\" } }");
    ServiceHost host(root_, "mystery");
    EXPECT_THROW(static_cast<void>(host.start()), std::invalid_argument);
}

TEST_F(ServiceHostTest, RequiredRegistrationFailureThrows) {
    const ScopedTlsEnvironment tls_environment;
    write(
        root_ / "services" / "login.lua",
        "return { " + transport_lua() +
            ", downstream_address = \"127.0.0.1\", downstream_port = 7100, "
            "service_discovery = { enabled = true, required = true, "
            "instance_id = \"login-test-01\", endpoint = "
            "\"http://127.0.0.1:1\", request_timeout_ms = 200, "
            "watch_interval_ms = 200 } }");
    ServiceHost host(root_, "login");
    EXPECT_THROW(static_cast<void>(host.start()), std::runtime_error);
    EXPECT_FALSE(host.ready());
    host.stop();  // 抛出后仍可安全关停。
}

TEST_F(ServiceHostTest, EscapesGaugeLabelSpecialCharacters) {
    const ScopedTlsEnvironment tls_environment;
    // instance_id 含引号与反斜杠(Lua 转义后为 we"ird\name)。
    write(
        root_ / "services" / "host_test.lua",
        "return { " + transport_lua() +
            ", service_discovery = { enabled = false, instance_id = "
            "\"we\\\"ird\\\\name\" } }");
    ServiceHost host(root_, "host_test");

    EXPECT_TRUE(host.start());
    // 标签值特殊字符转义为 \" 与 \\,保证 Prometheus 文本合法且值为 1。
    const auto metrics = host.prometheus_metrics();
    EXPECT_NE(metrics.find("\\\""), std::string::npos);
    EXPECT_NE(metrics.find("\\\\"), std::string::npos);
    EXPECT_NE(
        metrics.find("realmmesh_service_ready{service_name=\"host_test\","
                     "service_instance=\"we\\\"ird\\\\name\"} 1"),
        std::string::npos);

    host.stop();
}

}  // namespace
}  // namespace realm::service_host
