#pragma once

#include "realmmesh/game/gateway/gateway_config_loader.hpp"
#include "realmmesh/game/login_verify/login_verify_config.hpp"
#include "realmmesh/game/queue/queue_config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace realm::service_host {

/// CLI 覆盖项:全部可选,覆盖合并结果。
struct CliOverrides {
    std::optional<std::string> instance_id;
    std::optional<std::string> node_id;
    std::optional<std::string> zone;
};

/// 分层配置加载:configs/<root> 下 common/*.lua 深合并 services/<name>.lua,
/// CLI 覆盖最后生效;日志 file_path 按实例身份生成。
class LayeredConfigLoader final {
public:
    /// 消息服务(gateway/realm/login)的配置。
    [[nodiscard]] static game::gateway::GatewayConfig load(
        const std::filesystem::path& config_root,
        std::string_view service_name,
        const CliOverrides& overrides = {});

    /// 健全服配置:宿主级节段(logging、service_discovery、metrics)与
    /// login_verify 专属节段一次装载。
    struct LoginVerifyServiceConfig {
        game::gateway::GatewayConfig host;
        game::login_verify::LoginVerifyConfig login_verify;
    };

    /// login_verify 的账号集相对路径在此按 config_root 解析为绝对路径。
    [[nodiscard]] static LoginVerifyServiceConfig load_login_verify(
        const std::filesystem::path& config_root,
        std::string_view service_name,
        const CliOverrides& overrides = {});

    /// 排队调度服配置:宿主级节段(logging、service_discovery、metrics)
    /// 与 queue 专属节段一次装载。
    struct QueueServiceConfig {
        game::gateway::GatewayConfig host;
        game::queue::QueueConfig queue;
    };

    [[nodiscard]] static QueueServiceConfig load_queue(
        const std::filesystem::path& config_root,
        std::string_view service_name,
        const CliOverrides& overrides = {});
};

}  // namespace realm::service_host
