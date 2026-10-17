#pragma once

#include "realmmesh/service_host/layered_config.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace realm::service_host {

/// CLI 覆盖项:全部可选，覆盖合并结果。
struct CliOverrides {
    std::optional<std::string> instance_id;
    std::optional<std::string> node_id;
    std::optional<std::string> zone;
};

/// 分层配置加载:configs/<root> 下 common/*.lua 深合并 services/<name>.lua,
/// CLI 覆盖最后生效;日志 file_path 按实例身份生成。宿主级与接入面节段
/// 在此解析，非法取值抛 std::invalid_argument;服务层文件缺失或 Lua 出错
/// 抛 std::runtime_error。
class LayeredConfigLoader final {
public:
    [[nodiscard]] static LayeredConfig load(
        const std::filesystem::path& config_root,
        std::string_view service_name,
        const CliOverrides& overrides = {});
};

}  // namespace realm::service_host
