#include "realmmesh/game/gateway/gateway_login_config.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <stdexcept>

namespace realm::game::gateway {
namespace {

using namespace std::chrono_literals;

TEST(GatewayLoginConfigTest, AcceptsCompleteConfiguration) {
    GatewayLoginConfig config{
        .conn_capacity = 20,
        .fetch_capacity = 10,
        .fetch_retry_base = 2s,
        .fetch_retry_max = 3,
        .handoff_grace = 5s,
        .static_realm = RealmEndpoint{"127.0.0.1", 7100},
    };
    EXPECT_NO_THROW(config.validate());
}

TEST(GatewayLoginConfigTest, RejectsInvalidCapacityRetryAndEndpoint) {
    GatewayLoginConfig config;
    config.conn_capacity = 0;
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.conn_capacity = 1;
    config.fetch_retry_max = 11;
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.fetch_retry_max = 3;
    config.static_realm = RealmEndpoint{"", 7100};
    EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(GatewaySigningMaterialTest, RejectsMalformedInputsImmediately) {
    EXPECT_THROW(
        static_cast<void>(GatewaySigningMaterial::from_hex(
            "bad",
            "login-verify-v1",
            "bad",
            "queue-v1",
            "bad",
            "realmmesh/login-verify")),
        std::invalid_argument);
}

TEST(GatewaySigningMaterialTest, BuildsAllRequiredInputs) {
    const auto material = GatewaySigningMaterial::from_hex(
        "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60",
        "login-verify-v1",
        "4ccd089b28ff96da9db6c346ec114e0f5b8a319f35aba624da8cf6ed4fb8a6fb",
        "queue-v1",
        "0102030405060708090a0b0c0d0e0f10"
        "1112131415161718191a1b1c1d1e1f20",
        "realmmesh/login-verify");

    EXPECT_EQ(material.identity_kid, "login-verify-v1");
    EXPECT_EQ(material.queue_kid, "queue-v1");
    EXPECT_EQ(material.identity_issuer, "realmmesh/login-verify");
}

TEST(GatewaySigningMaterialTest, MissingEnvironmentFailsImmediately) {
    static_cast<void>(::unsetenv("REALMMESH_IDENTITY_KEY_SEED"));
    static_cast<void>(::unsetenv("REALMMESH_QUEUE_KEY_SEED"));
    static_cast<void>(::unsetenv("REALMMESH_SESSION_TICKET_KEY"));

    EXPECT_THROW(
        static_cast<void>(load_gateway_signing_material()),
        std::runtime_error);
}

}  // namespace
}  // namespace realm::game::gateway
