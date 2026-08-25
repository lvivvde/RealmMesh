#pragma once

#include <string>
#include <vector>

namespace realm::service_host {

/// 单服务拓扑描述:name 必须非空;depends_on 引用 specs 内其他 name。
struct ServiceSpec {
    std::string name;
    std::vector<std::string> depends_on;
    bool entry{false};  ///< 入口服务(全部 ready 后才放行)
};

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
