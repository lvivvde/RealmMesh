#pragma once

#include "realmmesh/network/transport/transport_config.hpp"
#include "realmmesh/scripting/lua_runtime.hpp"

#include <filesystem>
#include <string>

namespace realm::game::login_verify {

/// 健全服专属配置节(根表 `login_verify`);宿主级节段(logging、
/// service_discovery)仍由 GatewayConfigLoader::parse 承载,ServiceHost
/// 装配时两路合流。TLS 路径支持配置直填或经环境变量名解析(先例同
/// gateway 传输配置)。
struct LoginVerifyConfig {
    network::TransportConfig::TlsServerIdentity tls;
    std::string listen_address{"127.0.0.1"};
    std::uint16_t listen_port{8443};
    std::string kid{"login-verify-v1"};
    /// 相对 config_root(即 configs/ 目录)解析;默认即 configs/common/accounts.lua。
    std::filesystem::path accounts_file{"common/accounts.lua"};
};

class LoginVerifyConfigLoader final {
public:
    /// 解析已合并的 Lua 根表(供分层加载器复用);`login_verify` 节缺失
    /// 或字段类型错抛 std::invalid_argument。
    [[nodiscard]] static LoginVerifyConfig parse(const sol::table& root);
};

}  // namespace realm::game::login_verify
