// Gateway 轻量头不带 EdgeRuntime 的完整定义(#128):事件、启动配置与
// 主传输边界各自成头，GatewayPrimaryTransport 及登录管线、准入只前置声明
// runtime。本文件只包含这些头，任何一个经传递 include 引入
// edge_runtime.hpp 都在编译期失败。
#include "realmmesh/edge/edge_event.hpp"
#include "realmmesh/game/gateway/gateway_admission.hpp"
#include "realmmesh/game/gateway/gateway_config.hpp"
#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"
#include "realmmesh/game/gateway/gateway_primary_transport.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <vector>

namespace realm::game::gateway {
namespace {

template <typename T>
concept CompleteType = requires { sizeof(T); };

static_assert(
    !CompleteType<edge::EdgeRuntime>,
    "light gateway headers must not include edge_runtime.hpp");

TEST(GatewayHeadersTest, EventsFlowThroughPrimaryTransportWithoutRuntime) {
    InMemoryGatewayPrimaryTransport transport;
    transport.push_event(edge::EdgeEvent{
        .kind = edge::EdgeEventKind::MessageReceived,
        .connection_id = edge::EdgeConnectionId{7},
        .payload = std::vector<std::byte>{std::byte{0x2A}},
        .source = "127.0.0.1"});

    const std::vector<edge::EdgeEvent> events = transport.drain_events(4);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().kind, edge::EdgeEventKind::MessageReceived);
    EXPECT_EQ(events.front().connection_id, edge::EdgeConnectionId{7});
}

}  // namespace
}  // namespace realm::game::gateway
