// 普通配置头只声明配置值，不带 Lua(#126):本文件只包含配置值消费者
// 会用到的头，任何一个经传递 include 引入 sol2 都在编译期失败。Lua 解析
// 入口是各模块的 *_config_lua.hpp,由解析实现、分层装载器与解析测试显式选择。
// 训练规则头只前置声明 LuaRuntime(#127),同样受本守卫约束。
// Gateway 启动配置单独成头(#128):配置值消费者也不经传递 include 拿到
// EdgeRuntime 的完整定义(私有队列、线程与锁布局);服务模块、业务帧与通用
// 宿主头(ADR-0014)同样只前置声明它。
#include "realmmesh/game/common/player_data_config.hpp"
#include "realmmesh/game/gateway/gateway_config.hpp"
#include "realmmesh/game/gateway/gateway_frame.hpp"
#include "realmmesh/game/gateway/gateway_module.hpp"
#include "realmmesh/game/login_verify/login_verify_config.hpp"
#include "realmmesh/game/login_verify/login_verify_module.hpp"
#include "realmmesh/game/login_verify/login_verify_service.hpp"
#include "realmmesh/game/queue/queue_config.hpp"
#include "realmmesh/game/queue/queue_module.hpp"
#include "realmmesh/game/queue/queue_service.hpp"
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/game/realm/realm_frame.hpp"
#include "realmmesh/game/realm/realm_module.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/game/realm/training_rule.hpp"
#include "realmmesh/edge/edge_config.hpp"
#include "realmmesh/service_host/edge_service_module.hpp"
#include "realmmesh/service_host/layered_config.hpp"
#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/service_host/mesh_host.hpp"
#include "realmmesh/service_host/service_host.hpp"
#include "realmmesh/service_host/service_module.hpp"

#if defined(SOL_HPP) || defined(SOL_FORWARD_HPP)
#error "plain configuration headers must not include sol2"
#endif

#include <gtest/gtest.h>

#include <filesystem>

namespace realm::edge {
class EdgeRuntime;
}  // namespace realm::edge

namespace realm::service_host {
namespace {

template <typename T>
concept CompleteType = requires { sizeof(T); };

static_assert(
    !CompleteType<edge::EdgeRuntime>,
    "plain configuration headers must not include edge_runtime.hpp");

TEST(ConfigHeadersTest, ConfigValuesAreUsableWithoutLua) {
    game::realm::RealmConfig realm;
    realm.training_rule_file = "services/realm/training.lua";
    EXPECT_EQ(realm.data_workers, 4U);

    game::gateway::GatewayConfig gateway;
    EXPECT_EQ(gateway.tick_rate, 20U);

    edge::EdgeConfig edge;
    EXPECT_EQ(edge.runtime.inbound_capacity, 65'536U);
    EXPECT_EQ(edge.session_capacity(), 0U);

    HostConfig host;
    EXPECT_FALSE(host.service_discovery.enabled);

    game::queue::QueueConfig queue;
    EXPECT_EQ(queue.listen_port, 8444);

    game::login_verify::LoginVerifyConfig login_verify;
    EXPECT_EQ(login_verify.listen_port, 8443);

    game::common::PlayerDataConfig player_data;
    player_data.options.bootstrap_accounts_file = "common/accounts.lua";
    game::common::resolve_player_data_paths(player_data, "/configs");
    EXPECT_EQ(
        *player_data.options.bootstrap_accounts_file,
        std::filesystem::path("/configs/common/accounts.lua"));
}

}  // namespace
}  // namespace realm::service_host
