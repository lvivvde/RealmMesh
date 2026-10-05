// Gateway 轻量头不带 GatewayRuntime 的完整定义(#128):事件、启动配置与
// 主传输边界各自成头，GatewayPrimaryTransport 及登录管线、准入只前置声明
// runtime。本文件只包含这些头，任何一个经传递 include 引入
// gateway_runtime.hpp 都在编译期失败。
#include "realmmesh/game/gateway/gateway_admission.hpp"
#include "realmmesh/game/gateway/gateway_config.hpp"
#include "realmmesh/game/gateway/gateway_event.hpp"
#include "realmmesh/game/gateway/gateway_login_pipeline.hpp"
#include "realmmesh/game/gateway/gateway_primary_transport.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstddef>
#include <vector>

namespace realm::game::gateway {
namespace {

template <typename T>
concept CompleteType = requires { sizeof(T); };

static_assert(
    !CompleteType<GatewayRuntime>,
    "light gateway headers must not include gateway_runtime.hpp");

TEST(GatewayHeadersTest, EventsFlowThroughPrimaryTransportWithoutRuntime) {
    InMemoryGatewayPrimaryTransport transport;
    transport.push_event(GatewayEvent{
        .kind = GatewayEventKind::MessageReceived,
        .session_id = EdgeSessionId{7},
        .payload = std::vector<std::byte>{std::byte{0x2A}},
        .source = "127.0.0.1"});

    const std::vector<GatewayEvent> events = transport.drain_events(4);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events.front().kind, GatewayEventKind::MessageReceived);
    EXPECT_EQ(events.front().session_id, EdgeSessionId{7});
    EXPECT_FALSE(events.front().established);
}

TEST(GatewayHeadersTest, StartupConfigIsUsableWithoutRuntime) {
    GatewayConfig config;
    EXPECT_EQ(config.tick_rate, 20U);
    EXPECT_EQ(config.max_events_per_frame, 4'096U);
    EXPECT_EQ(config.runtime.outbound_capacity, 65'536U);
    EXPECT_EQ(config.runtime.io_poll_interval, std::chrono::milliseconds{2});
}

}  // namespace
}  // namespace realm::game::gateway
