#include "realmmesh/game/gateway/gateway_config_loader.hpp"
#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>

namespace realm::game::gateway {
namespace {

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
    }
    ~ScopedTlsEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
    }
};

/// 拷贝权威 configs/ 的两层输入(common/ 与 services/)到临时目录:
/// LayeredConfigLoader 会把日志写进 <root>/logs/,拷贝避免污染源码树。
void copy_layered_configs(
    const std::filesystem::path& source,
    const std::filesystem::path& target) {
    std::error_code error;
    std::filesystem::copy(
        source / "common",
        target / "common",
        std::filesystem::copy_options::recursive,
        error);
    ASSERT_FALSE(error) << "failed to copy common layer: " << error.message();
    std::filesystem::copy(
        source / "services",
        target / "services",
        std::filesystem::copy_options::recursive,
        error);
    ASSERT_FALSE(error) << "failed to copy services layer: " << error.message();
}

/// 单文件加载路径的真实输入:夹具刻意保留分层前的单体形态,因为分层后的
/// services 层缺少 file_path、队列容量等字段,无法通过完整性校验。
TEST(GatewayConfigLoaderTest, LoadsQuicPrimaryAndTlsTcpFallbackFromLua) {
    const ScopedTlsEnvironment tls_environment;
    const auto config = GatewayConfigLoader::load(
        std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) /
        "tests/cpp/game/gateway/fixtures/gateway_single_file.lua");

    ASSERT_EQ(config.transports.size(), 2U);
    EXPECT_EQ(config.tick_rate, 20U);
    EXPECT_TRUE(config.service_discovery.enabled);
    EXPECT_EQ(config.runtime.inbound_capacity, 65536U);
    EXPECT_EQ(config.logging.min_severity, observability::Severity::Info);
    EXPECT_EQ(
        config.logging.file_path,
        std::filesystem::path(".runtime/logs/gateway/gateway-dev-01.jsonl"));
    EXPECT_EQ(config.logging.normal_queue_capacity, 8192U);
    EXPECT_EQ(config.logging.priority_queue_capacity, 2048U);
    ASSERT_EQ(config.logging.module_levels.size(), 1U);
    EXPECT_EQ(
        config.logging.module_levels.at("framework.network"),
        observability::Severity::Warn);
    ASSERT_EQ(config.logging.sample_rates.size(), 1U);
    EXPECT_DOUBLE_EQ(config.logging.sample_rates.at("malformed_request"), 0.1);
    EXPECT_EQ(config.logging_metrics.listen_address, "127.0.0.1");
    EXPECT_EQ(config.logging_metrics.port, 9103U);
    EXPECT_EQ(config.logging_identity.environment, "development");
    EXPECT_EQ(config.logging_identity.cluster, "local");
    EXPECT_EQ(config.logging_identity.service_name, "gateway");
    EXPECT_EQ(config.logging_identity.service_instance, "gateway-dev-01");
    EXPECT_EQ(config.logging_identity.node_id, "development-node");

    const auto& quic = config.transports[0];
    const auto& tls_tcp = config.transports[1];
    EXPECT_EQ(quic.name, "client_quic");
    EXPECT_EQ(quic.protocol, network::TransportProtocol::Quic);
    EXPECT_EQ(tls_tcp.name, "client_tls_tcp");
    EXPECT_EQ(tls_tcp.protocol, network::TransportProtocol::TlsTcp);
    EXPECT_TRUE(quic.enabled);
    EXPECT_TRUE(tls_tcp.enabled);
    EXPECT_EQ(quic.listen_port, tls_tcp.listen_port);
    EXPECT_EQ(quic.listen_port, 8000U);
    EXPECT_EQ(quic.handshake_timeout, std::chrono::milliseconds(3000));
    EXPECT_EQ(tls_tcp.handshake_timeout, std::chrono::milliseconds(3000));
    ASSERT_TRUE(quic.tls.has_value());
    ASSERT_TRUE(tls_tcp.tls.has_value());
    EXPECT_EQ(quic.tls->alpn, "realmmesh-edge/1");
    EXPECT_EQ(quic.tls->certificate_chain_file, REALMMESH_TEST_TLS_CERTIFICATE);
    EXPECT_EQ(tls_tcp.tls->private_key_file, REALMMESH_TEST_TLS_PRIVATE_KEY);
}

/// 生产路径:分层加载权威 configs/ 树。断言的是真实合并结果,而不是夹具里
/// 固化过的旧值——服务层覆盖公共层(network 模块 info、发现 enabled=false)。
TEST(LayeredConfigLoaderTest, MergesAuthoritativeGatewayTreeFieldByField) {
    const ScopedTlsEnvironment tls_environment;
    const test_support::TemporaryDirectory configs("realmmesh-gateway-config-");
    copy_layered_configs(
        std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs",
        configs.path());

    const auto config =
        service_host::LayeredConfigLoader::load(configs.path(), "gateway");

    // 公共层(configs/common/logging.lua)提供的基础字段。
    EXPECT_EQ(config.logging.min_severity, observability::Severity::Info);
    EXPECT_EQ(config.logging.normal_queue_capacity, 8192U);
    EXPECT_EQ(config.logging.priority_queue_capacity, 2048U);
    EXPECT_EQ(config.logging.max_file_size, 128U * 1024U * 1024U);
    EXPECT_EQ(config.logging.retained_files, 8U);
    EXPECT_TRUE(config.logging.console);

    // 服务层覆盖公共层:framework.network 在 services/gateway.lua 里是 info
    // (夹具的旧值是 warn)。
    ASSERT_EQ(config.logging.module_levels.size(), 1U);
    EXPECT_EQ(
        config.logging.module_levels.at("framework.network"),
        observability::Severity::Info);
    ASSERT_EQ(config.logging.sample_rates.size(), 1U);
    EXPECT_DOUBLE_EQ(config.logging.sample_rates.at("malformed_request"), 0.1);

    // 日志文件路径由加载器按实例身份生成到临时树内,不落到源码树。
    EXPECT_EQ(
        config.logging.file_path,
        configs.path() / "logs" / "gateway" / "gateway-gateway-dev-01.jsonl");

    // 身份:environment/cluster/region 来自公共层,service_instance 等由
    // 服务发现身份回填。
    EXPECT_EQ(config.logging_identity.environment, "development");
    EXPECT_EQ(config.logging_identity.cluster, "local");
    EXPECT_EQ(config.logging_identity.region, "local");
    EXPECT_EQ(config.logging_identity.service_name, "gateway");
    EXPECT_EQ(config.logging_identity.service_instance, "gateway-dev-01");
    EXPECT_EQ(config.logging_identity.node_id, "development-node");
    EXPECT_EQ(config.logging_identity.zone, "development");

    // 公共层(configs/common/discovery.lua)把发现默认关闭——与夹具的 true
    // 相反,这才是权威树的值。
    EXPECT_FALSE(config.service_discovery.enabled);
    EXPECT_FALSE(config.service_discovery.required);
    EXPECT_EQ(config.service_discovery.endpoint, "http://127.0.0.1:2379");
    EXPECT_EQ(config.service_discovery.key_prefix, "/realmmesh/services");
    EXPECT_EQ(config.service_discovery.instance_id, "gateway-dev-01");
    EXPECT_EQ(config.service_discovery.node_id, "development-node");
    EXPECT_EQ(config.service_discovery.zone, "development");
    EXPECT_EQ(config.service_discovery.advertise_address, "127.0.0.1");
    EXPECT_EQ(config.service_discovery.lease_ttl, std::chrono::seconds(15));
    EXPECT_EQ(
        config.service_discovery.request_timeout,
        std::chrono::milliseconds(500));
    EXPECT_EQ(
        config.service_discovery.watch_interval,
        std::chrono::milliseconds(500));
    EXPECT_EQ(
        config.service_discovery.startup_timeout,
        std::chrono::milliseconds(5000));

    // 服务层其余字段逐项校验。
    EXPECT_EQ(config.logging_metrics.listen_address, "127.0.0.1");
    EXPECT_EQ(config.logging_metrics.port, 9103U);
    EXPECT_EQ(config.tick_rate, 20U);
    EXPECT_EQ(config.max_events_per_frame, 4096U);
    EXPECT_EQ(config.runtime.inbound_capacity, 65536U);
    EXPECT_EQ(config.runtime.outbound_capacity, 65536U);
    EXPECT_EQ(config.runtime.max_commands_per_cycle, 4096U);
    EXPECT_EQ(config.runtime.io_poll_interval, std::chrono::milliseconds(2));

    ASSERT_EQ(config.transports.size(), 2U);
    const auto& quic = config.transports[0];
    const auto& tls_tcp = config.transports[1];
    EXPECT_EQ(quic.name, "client_quic");
    EXPECT_EQ(quic.protocol, network::TransportProtocol::Quic);
    EXPECT_EQ(tls_tcp.name, "client_tls_tcp");
    EXPECT_EQ(tls_tcp.protocol, network::TransportProtocol::TlsTcp);
    EXPECT_TRUE(quic.enabled);
    EXPECT_TRUE(tls_tcp.enabled);
    EXPECT_EQ(quic.listen_address, "0.0.0.0");
    EXPECT_EQ(quic.listen_port, 8000U);
    EXPECT_EQ(tls_tcp.listen_port, 8000U);
    EXPECT_EQ(quic.max_sessions, 10000U);
    EXPECT_EQ(quic.max_payload_size, 65536U);
    EXPECT_EQ(quic.max_pending_output_bytes, 4194304U);
    EXPECT_EQ(quic.handshake_timeout, std::chrono::milliseconds(3000));
    EXPECT_EQ(quic.idle_timeout, std::chrono::milliseconds(30000));
    ASSERT_TRUE(quic.tls.has_value());
    ASSERT_TRUE(tls_tcp.tls.has_value());
    EXPECT_EQ(quic.tls->alpn, "realmmesh-edge/1");
    EXPECT_EQ(quic.tls->certificate_chain_file, REALMMESH_TEST_TLS_CERTIFICATE);
    EXPECT_EQ(tls_tcp.tls->private_key_file, REALMMESH_TEST_TLS_PRIVATE_KEY);
}

/// CLI 覆盖优先级最高,并参与日志文件名的实例身份。
TEST(LayeredConfigLoaderTest, CliOverrideWinsAndNamesTheLogFile) {
    const ScopedTlsEnvironment tls_environment;
    const test_support::TemporaryDirectory configs("realmmesh-gateway-config-");
    copy_layered_configs(
        std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) / "configs",
        configs.path());

    service_host::CliOverrides overrides;
    overrides.instance_id = "gateway-test-07";
    overrides.node_id = "test-node";
    const auto config = service_host::LayeredConfigLoader::load(
        configs.path(), "gateway", overrides);

    EXPECT_EQ(config.service_discovery.instance_id, "gateway-test-07");
    EXPECT_EQ(config.service_discovery.node_id, "test-node");
    EXPECT_EQ(config.logging_identity.service_instance, "gateway-test-07");
    EXPECT_EQ(config.logging_identity.node_id, "test-node");
    EXPECT_EQ(
        config.logging.file_path,
        configs.path() / "logs" / "gateway" / "gateway-gateway-test-07.jsonl");
}

}  // namespace
}  // namespace realm::game::gateway
