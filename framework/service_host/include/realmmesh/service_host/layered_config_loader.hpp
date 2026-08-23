#pragma once

#include "realmmesh/game/gateway/gateway_config_loader.hpp"

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
    /// config_root: 含 common/ 与 services/ 的目录。文件缺失/解析失败抛异常。
    [[nodiscard]] static game::gateway::GatewayConfig load(
        const std::filesystem::path& config_root,
        std::string_view service_name,
        const CliOverrides& overrides = {});
};

}  // namespace realm::service_host
