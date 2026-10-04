#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace realm::service_host {

/// 单服务拓扑描述:name 必须非空;depends_on 引用 specs 内其他 name。
struct ServiceSpec {
    std::string name;
    std::vector<std::string> depends_on;
    bool entry{false};  ///< 入口服务(全部 ready 后才放行)
};

/// 拓扑装载入口(#127):读取 <config_root>/main.config 的 services 表，
/// 按声明序返回拓扑描述;Lua 只在实现文件里。文件读取或执行失败抛
/// std::runtime_error;services 表缺失或为空、条目格式错抛
/// std::invalid_argument。依赖关系的校验仍由 StartupTopology 负责。
[[nodiscard]] std::vector<ServiceSpec> load_topology(
    const std::filesystem::path& config_root);

/// 拓扑解析:环、未知依赖、重名、entry 被其他服务依赖 →
/// 抛 std::invalid_argument;entry 服务统一放在全图最终波次。
class StartupTopology final {
public:
    explicit StartupTopology(std::vector<ServiceSpec> specs);

    /// 启动波次:每波内服务可并行,波间必须等待前波 ready。
    [[nodiscard]] const std::vector<std::vector<std::string>>& waves()
        const noexcept;

    /// 关停顺序:启动完成序的严格反序(同波内保持声明序反转)。
    [[nodiscard]] std::vector<std::string> shutdown_order() const;

    /// 全部服务的 ready 才允许 entry 服务放行。
    [[nodiscard]] const std::vector<std::string>& all_names() const noexcept;

private:
    std::vector<std::vector<std::string>> waves_;
    std::vector<std::string> all_names_;
};

}  // namespace realm::service_host
