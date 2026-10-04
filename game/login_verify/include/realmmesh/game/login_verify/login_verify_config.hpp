#pragma once

#include "realmmesh/game/common/player_data_store.hpp"
#include "realmmesh/network/transport/transport_config.hpp"

#include <cstddef>
#include <filesystem>
#include <string>

namespace realm::game::login_verify {

/// 健全服专属配置节(根表 `login_verify`);宿主级节段(logging、
/// service_discovery)仍由 parse_gateway_config 承载,ServiceHost
/// 装配时两路合流。TLS 路径支持配置直填或经环境变量名解析(先例同
/// gateway 传输配置)。
struct LoginVerifyConfig {
    network::TransportConfig::TlsServerIdentity tls;
    std::string listen_address{"127.0.0.1"};
    std::uint16_t listen_port{8443};
    std::string kid{"login-verify-v1"};
    /// 相对 config_root(即 configs/ 目录)解析;默认即 configs/common/accounts.lua。
    std::filesystem::path accounts_file{"common/accounts.lua"};
    /// uri 非空时使用 MongoDB 权威数据源(ADR-0011),accounts_file 不再参与
    /// 认证；Lua 账号表只经 player_data.bootstrap_accounts_file 导入空库。
    common::PlayerDataConfig player_data;
    /// 验签工作者(#98):Argon2 + 账号源查询在这些线程上跑,不占 poll 线程;
    /// verify_capacity 是排队 + 运行中 + 未取走结果的上限,满额回 503。
    std::size_t verify_workers{4};
    std::size_t verify_capacity{64};
};

}  // namespace realm::game::login_verify
