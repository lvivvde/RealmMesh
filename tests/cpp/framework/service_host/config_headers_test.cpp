// 普通配置头只声明配置值，不带 Lua(#126):本文件只包含配置值消费者
// 会用到的头，任何一个经传递 include 引入 sol2 都在编译期失败。Lua 解析
// 入口是各模块的 *_config_lua.hpp,由解析实现、分层装载器与解析测试显式选择。
#include "realmmesh/game/common/player_data_config.hpp"
#include "realmmesh/game/gateway/gateway_config_loader.hpp"
#include "realmmesh/game/login_verify/login_verify_config.hpp"
#include "realmmesh/game/login_verify/login_verify_service.hpp"
#include "realmmesh/game/queue/queue_config.hpp"
#include "realmmesh/game/queue/queue_service.hpp"
#include "realmmesh/game/realm/realm_config.hpp"
#include "realmmesh/game/realm/realm_sessions.hpp"
#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/service_host/mesh_host.hpp"

#if defined(SOL_HPP) || defined(SOL_FORWARD_HPP)
#error "plain configuration headers must not include sol2"
#endif

#include <gtest/gtest.h>

#include <filesystem>

namespace realm::service_host {
namespace {

TEST(ConfigHeadersTest, ConfigValuesAreUsableWithoutLua) {
    LayeredConfigLoader::RealmServiceConfig realm;
    realm.realm.training_rule_file = "services/realm/training.lua";
    EXPECT_EQ(realm.realm.data_workers, 4U);

    LayeredConfigLoader::QueueServiceConfig queue;
    EXPECT_EQ(queue.queue.listen_port, 8444);

    LayeredConfigLoader::LoginVerifyServiceConfig login_verify;
    EXPECT_EQ(login_verify.login_verify.listen_port, 8443);

    game::common::PlayerDataConfig player_data;
    player_data.options.bootstrap_accounts_file = "common/accounts.lua";
    game::common::resolve_player_data_paths(player_data, "/configs");
    EXPECT_EQ(
        *player_data.options.bootstrap_accounts_file,
        std::filesystem::path("/configs/common/accounts.lua"));
}

}  // namespace
}  // namespace realm::service_host
