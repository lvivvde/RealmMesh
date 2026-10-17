// 连接运行时头只依赖来源地址归一化(ADR-0014 第 2 步):本文件只包含
// gateway_runtime.hpp,它经传递 include 拿到凭据入口、登录配置或
// EdgeAttach 的完整定义都在编译期失败。与 gateway_headers_test.cpp 分属
// 两个编译单元，各自守住自己的 include 集合。
#include "realmmesh/game/gateway/gateway_runtime.hpp"

#include <gtest/gtest.h>

namespace realmmesh::protocol::edge::v1 {
class EdgeAttach;
}  // namespace realmmesh::protocol::edge::v1

namespace realm::game::gateway {
class GatewayCredentialIngress;
struct GatewayCredentialIngressConfig;
struct GatewayLoginConfig;

namespace {

template <typename T>
concept CompleteType = requires { sizeof(T); };

static_assert(
    !CompleteType<GatewayCredentialIngress> &&
        !CompleteType<GatewayCredentialIngressConfig>,
    "gateway_runtime.hpp must not include gateway_credential_ingress.hpp");
static_assert(
    !CompleteType<GatewayLoginConfig> && !CompleteType<GatewayConfig>,
    "gateway_runtime.hpp must not include gateway_config.hpp");
static_assert(
    !CompleteType<::realmmesh::protocol::edge::v1::EdgeAttach>,
    "gateway_runtime.hpp must not include game/common edge protocol");

TEST(GatewayRuntimeHeadersTest, SourceNormalizationIsReachableWithoutCredentials) {
    const GatewaySourceNormalizer normalizer(GatewaySourceConfig{});
    EXPECT_EQ(
        normalizer.normalize({.direct_peer = "192.0.2.10"}), "192.0.2.10");
}

}  // namespace
}  // namespace realm::game::gateway
