#pragma once

// 分层装载的结果(ADR-0014):宿主级节段与接入面节段已解析为配置值;
// 各服务模块的专属节段留在合并后的 Lua 根表里，由模块经
// layered_config_lua.hpp 自行解析。本头不经 include 传递 sol2。
#include "realmmesh/edge/edge_config.hpp"
#include "realmmesh/service_host/host_config.hpp"

#include <filesystem>
#include <memory>
#include <string>

namespace realm::service_host {

namespace detail {
struct LayeredConfigAccess;
}  // namespace detail

class LayeredConfig final {
public:
    LayeredConfig(LayeredConfig&&) noexcept;
    LayeredConfig& operator=(LayeredConfig&&) noexcept;
    ~LayeredConfig();

    LayeredConfig(const LayeredConfig&) = delete;
    LayeredConfig& operator=(const LayeredConfig&) = delete;

    [[nodiscard]] const std::filesystem::path& config_root() const noexcept;
    [[nodiscard]] const std::string& service_name() const noexcept;
    /// 服务发现与日志节段。
    [[nodiscard]] const HostConfig& host() const noexcept;
    /// 接入面节段(transports、runtime、ingress_source);不监听的服务为空。
    [[nodiscard]] const edge::EdgeConfig& edge() const noexcept;
    /// 相对路径按 config_root 解析为绝对路径，绝对路径原样返回。
    [[nodiscard]] std::filesystem::path resolve_path(
        const std::filesystem::path& path) const;

private:
    friend class LayeredConfigLoader;
    friend struct detail::LayeredConfigAccess;
    struct State;
    explicit LayeredConfig(std::unique_ptr<State> state);
    std::unique_ptr<State> state_;
};

}  // namespace realm::service_host
