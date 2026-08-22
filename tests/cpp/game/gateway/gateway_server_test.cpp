#include "realmmesh/game/gateway/gateway_config_loader.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>

namespace realm::game::gateway {
namespace {

class ScopedTlsEnvironment final {
public:
    ScopedTlsEnvironment() {
        EXPECT_EQ(::setenv(
            "REALMMESH_TLS_CERTIFICATE_FILE",
            REALMMESH_TEST_TLS_CERTIFICATE,
            1), 0);
        EXPECT_EQ(::setenv(
            "REALMMESH_TLS_PRIVATE_KEY_FILE",
            REALMMESH_TEST_TLS_PRIVATE_KEY,
            1), 0);
    }
    ~ScopedTlsEnvironment() {
        static_cast<void>(::unsetenv("REALMMESH_TLS_CERTIFICATE_FILE"));
        static_cast<void>(::unsetenv("REALMMESH_TLS_PRIVATE_KEY_FILE"));
    }
};

TEST(GatewayConfigLoaderTest, LoadsQuicPrimaryAndTlsTcpFallbackFromLua) {
    const ScopedTlsEnvironment tls_environment;
    const auto config = GatewayConfigLoader::load(
        std::filesystem::path(REALMMESH_TEST_SOURCE_DIR) /
        "lua/config/services/gateway.lua");

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
    EXPECT_DOUBLE_EQ(
        config.logging.sample_rates.at("malformed_request"), 0.1);
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
    EXPECT_EQ(quic.tls->certificate_chain_file,
              REALMMESH_TEST_TLS_CERTIFICATE);
    EXPECT_EQ(tls_tcp.tls->private_key_file,
              REALMMESH_TEST_TLS_PRIVATE_KEY);
}

}  // namespace
}  // namespace realm::game::gateway
