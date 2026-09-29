#include "realmmesh/game/gateway/gateway_login_config.hpp"

#include "realmmesh/game/common/admission_grant.hpp"
#include "realmmesh/game/gateway/gateway_config_loader.hpp"
#include "realmmesh/scripting/lua_runtime.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <stdexcept>
#include <string>
#include <string_view>

namespace realm::game::gateway {
namespace {

using namespace std::chrono_literals;

/// 解析一段 Lua 根表:走与生产同一入口 GatewayConfigLoader::parse,
/// 不复制字段名。transports 与 logging 是 loader 的必填节,给最小合法值
/// (TLS 路径在 parse 期只解析不打开文件);其余字段吃生产默认值。
[[nodiscard]] GatewayConfig parse_lua(std::string_view admission_block) {
    const std::string source =
        "return { transports = { { name = \"client_tls_tcp\", protocol = "
        "\"tls_tcp\", enabled = true, max_sessions = 16, "
        "certificate_chain_file = \"/dev/null\", private_key_file = "
        "\"/dev/null\" } }, logging = { service_name = \"gateway\", file_path "
        "= \"/tmp/realmmesh-gateway-config-test.jsonl\" }, admission = {" +
        std::string{admission_block} + "} }";
    scripting::LuaRuntime runtime;
    std::string error;
    if (!runtime.load_module_source("gateway_config", source, &error)) {
        throw std::runtime_error("lua load failed: " + error);
    }
    return GatewayConfigLoader::parse(runtime.module("gateway_config"));
}

/// 一份能通过校验的完整登录配置:负例只在它之上改一个字段,免得断言
/// 被"另一个字段本来就不合法"掩盖。
[[nodiscard]] GatewayLoginConfig valid_config() {
    GatewayLoginConfig config;
    config.conn_capacity = 20;
    config.fetch_capacity = 10;
    config.fetch_retry_base = 2s;
    config.fetch_retry_max = 3;
    config.handoff_grace = 5s;
    config.static_realm = RealmEndpoint{"127.0.0.1", 7100};
    return config;
}

TEST(GatewayLoginConfigTest, AcceptsCompleteConfiguration) {
    GatewayLoginConfig config = valid_config();
    config.admission_grant_keys = {
        {.kid = "admission-grant-v2",
         .public_key_environment = "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY_V2"}};
    config.admission_grant_window = 300s;
    EXPECT_NO_THROW(config.validate());
}

TEST(GatewayLoginConfigTest, RejectsInvalidCapacityRetryAndEndpoint) {
    GatewayLoginConfig config = valid_config();
    config.conn_capacity = 0;
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.conn_capacity = 1;
    config.fetch_retry_max = 11;
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.fetch_retry_max = 3;
    config.static_realm = RealmEndpoint{"", 7100};
    EXPECT_THROW(config.validate(), std::invalid_argument);
}

TEST(GatewayLoginConfigTest, RejectsIncompleteAdmissionConfiguration) {
    GatewayLoginConfig config = valid_config();
    config.admission_grant_issuer.clear();
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.admission_grant_issuer = "realmmesh/queue";
    config.deployment_id.clear();
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.deployment_id = "development";
    config.admission_consumption_prefix.clear();
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.admission_consumption_prefix = "/realmmesh/admission/consumption";
    config.admission_reservation_ttl = 0s;
    EXPECT_THROW(config.validate(), std::invalid_argument);
}

/// 验证键环:kid 必须唯一且每条都指名公钥所在的环境变量。空环意味着
/// 没有任何 Grant 能被验证——静默放行等于关掉准入,必须启动即拒。
TEST(GatewayLoginConfigTest, RejectsMalformedAdmissionKeyRing) {
    GatewayLoginConfig config = valid_config();
    ASSERT_EQ(config.admission_grant_keys.size(), 1U);

    config.admission_grant_keys.clear();
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.admission_grant_keys = {
        {.kid = "admission-grant-v1", .public_key_environment = ""}};
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.admission_grant_keys = {
        {.kid = "", .public_key_environment = "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY"}};
    EXPECT_THROW(config.validate(), std::invalid_argument);

    // 轮换重叠期允许活动键 + 退休键并存,但 kid 不得重复。
    config.admission_grant_keys = {
        {.kid = "admission-grant-v1",
         .public_key_environment = "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY"},
        {.kid = "admission-grant-v1",
         .public_key_environment = "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY_OLD"}};
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.admission_grant_keys = {
        {.kid = "admission-grant-v1",
         .public_key_environment = "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY"},
        {.kid = "admission-grant-v0",
         .public_key_environment = "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY_OLD"}};
    EXPECT_NO_THROW(config.validate());
}

/// 配置可以收紧但不能抬高协议硬上限(admission_grant_max_window):窗口
/// 大于上限会让 Queue 的合法签发在网关侧被误判为过期。
TEST(GatewayLoginConfigTest, RejectsGrantWindowOutsideProtocolLimits) {
    GatewayLoginConfig config = valid_config();
    config.admission_grant_window = 0s;
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.admission_grant_window = common::admission_grant_max_window + 1s;
    EXPECT_THROW(config.validate(), std::invalid_argument);

    config.admission_grant_window = common::admission_grant_max_window;
    EXPECT_NO_THROW(config.validate());
}

/// 退役的单 kid 键名一律拒绝:静默忽略会让运维以为改生效了,而网关仍在
/// 用别的 kid 验签(见 #79 的 cutover 要求)。
TEST(GatewayLoginConfigTest, LoaderRejectsRetiredSingleKidKeyName) {
    EXPECT_THROW(
        static_cast<void>(parse_lua(
            "admission_grant_kid = \"admission-grant-v1\"")),
        std::invalid_argument);
}

/// 键环从 Lua 装载:轮换重叠期两条都要进环,窗口按秒解析。
TEST(GatewayLoginConfigTest, LoaderParsesKeyRingForRotationOverlap) {
    const auto config = parse_lua(
        "grant_keys = { { kid = \"admission-grant-v2\", "
        "public_key_environment = \"REALMMESH_ADMISSION_GRANT_PUBLIC_KEY\" }, "
        "{ kid = \"admission-grant-v1\", public_key_environment = "
        "\"REALMMESH_ADMISSION_GRANT_PUBLIC_KEY_OLD\" } }, "
        "grant_window_seconds = 600");
    ASSERT_EQ(config.login.admission_grant_keys.size(), 2U);
    EXPECT_EQ(config.login.admission_grant_keys.at(0).kid, "admission-grant-v2");
    EXPECT_EQ(
        config.login.admission_grant_keys.at(0).public_key_environment,
        "REALMMESH_ADMISSION_GRANT_PUBLIC_KEY");
    EXPECT_EQ(config.login.admission_grant_keys.at(1).kid, "admission-grant-v1");
    EXPECT_EQ(config.login.admission_grant_window, 600s);
    EXPECT_NO_THROW(config.login.validate());
}

}  // namespace
}  // namespace realm::game::gateway
