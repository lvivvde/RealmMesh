#include "realmmesh/service_host/service_host.hpp"

#include "realmmesh/game/gateway/gateway_runtime.hpp"
#include "realmmesh/network/tcp/tcp_listener.hpp"
#include "realmmesh/test_support/mongod_process.hpp"

#include <gtest/gtest.h>
#include <httplib.h>

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

[[nodiscard]] std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

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
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_IDENTITY_KEY_SEED",
                "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY",
                "207a067892821e25d770f1fba0c47c11ff4b813e54162ece9eb839e076231ab6",
                1),
            0);
        EXPECT_EQ(
            ::setenv(
                "REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY",
                "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210",
                1),
            0);
    }
    ~ScopedTlsEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));
        static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
        static_cast<void>(::unsetenv("REALMMESH_ADMISSION_GRANT_PUBLIC_KEY"));
        static_cast<void>(
            ::unsetenv("REALMMESH_ADMISSION_CONSUMPTION_DIGEST_KEY"));
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
        metrics.find(
            "realmmesh_service_ready{service_name=\"host_test\","
            "service_instance=\"host-test-01\"} 1"),
        std::string::npos);

    host.stop();
    EXPECT_FALSE(host.runtime().running());
    host.stop();  // 幂等:重复关停无害。
}

TEST_F(ServiceHostTest, RuntimeStopClearsReadinessOnNextTick) {
    const ScopedTlsEnvironment tls_environment;
    ServiceHost host(root_, "host_test");

    ASSERT_TRUE(host.start());
    ASSERT_TRUE(host.ready());

    host.runtime().stop();
    host.tick();

    EXPECT_FALSE(host.ready());
}

/// 网关装配的环境契约:#79 起准入凭据是 Admission Grant,装配需要
/// Admission 消费摘要键、验证键环里每个 kid 指名的公钥、身份 Token 种子
/// 与会话票据键。本用例只抽掉身份种子,断言失败信息点到该变量——即前置
/// 的 admission 材料已经就绪,失败点是确定的那一个。
TEST_F(ServiceHostTest, GatewayIdentitySeedFailsBeforeRuntimeConstruction) {
    const ScopedTlsEnvironment tls_environment;
    write(
        root_ / "services" / "gateway.lua",
        "return { " + transport_lua() + " }");
    ASSERT_EQ(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"), 0);

    try {
        ServiceHost host(root_, "gateway");
        FAIL() << "missing Gateway signing material must fail construction";
    } catch (const std::runtime_error& error) {
        EXPECT_NE(
            std::string_view(error.what())
                .find("REALMMESH_IDENTITY_KEY_SEED is not set"),
            std::string_view::npos);
    }
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

/// 注册被声明为必需而 etcd 不可达 → 启动抛错,且不留下半启动的日志痕迹。
/// 用 realm 承载:它的配置是扁平形态(transports + service_discovery),
/// 与 fixture 里写出的服务配置同形。
TEST_F(ServiceHostTest, RequiredRegistrationFailureThrows) {
    const ScopedTlsEnvironment tls_environment;
    const auto& mongod = test_support::MongodProcess::shared();
    write(
        root_ / "services" / "realm.lua",
        "return { " + transport_lua() +
            ", downstream_address = \"127.0.0.1\", downstream_port = 8000, "
            "player_data = { uri = \"" + mongod.uri() + "\", database = \"" +
            test_support::MongodProcess::fresh_database() + "\" }, "
            "service_discovery = { enabled = true, required = true, "
            "instance_id = \"realm-test-01\", endpoint = "
            "\"http://127.0.0.1:1\", request_timeout_ms = 200, "
            "watch_interval_ms = 200 } }");
    ServiceHost host(root_, "realm");
    EXPECT_THROW(static_cast<void>(host.start()), std::runtime_error);
    EXPECT_FALSE(host.ready());
    host.stop();  // 抛出后仍可安全关停。
    // 失败启动未写 service_started,关停不得补写无配对的 service_stopped。
    const auto log =
        read_file(root_ / "logs" / "realm" / "realm-realm-test-01.jsonl");
    EXPECT_EQ(
        log.find("\"event_name\":\"service_stopped\""), std::string::npos);
    EXPECT_EQ(
        log.find("\"event_name\":\"service_started\""), std::string::npos);
}

/// Realm 没有不复核角色归属的装配:缺 player_data.uri 即构造失败。
TEST_F(ServiceHostTest, RealmWithoutPlayerDataThrows) {
    const ScopedTlsEnvironment tls_environment;
    write(
        root_ / "services" / "realm.lua",
        "return { " + transport_lua() +
            ", downstream_address = \"127.0.0.1\", downstream_port = 8000 }");
    EXPECT_THROW(ServiceHost(root_, "realm"), std::invalid_argument);
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
        metrics.find(
            "realmmesh_service_ready{service_name=\"host_test\","
            "service_instance=\"we\\\"ird\\\\name\"} 1"),
        std::string::npos);

    host.stop();
}

TEST_F(ServiceHostTest, HttpMetricsExposeServiceReadiness) {
    const ScopedTlsEnvironment tls_environment;
    std::uint16_t metrics_port = 0;
    {
        const network::TcpListener reservation("127.0.0.1", 0);
        metrics_port = reservation.local_port();
    }
    write(
        root_ / "services" / "host_test.lua",
        "return { " + transport_lua() +
            ", logging = { metrics_listen_address = \"127.0.0.1\", "
            "metrics_port = " +
            std::to_string(metrics_port) + " } }");

    ServiceHost host(root_, "host_test");
    ASSERT_TRUE(host.start());
    httplib::Client client("127.0.0.1", metrics_port);
    const auto response = client.Get("/metrics");
    ASSERT_TRUE(response);
    ASSERT_EQ(response->status, 200);
    EXPECT_NE(
        response->body.find(
            "realmmesh_service_ready{service_name=\"host_test\","
            "service_instance=\"host-test-01\"} 1"),
        std::string::npos);
    host.stop();
}

}  // namespace
}  // namespace realm::service_host
