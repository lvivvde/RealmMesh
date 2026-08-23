#pragma once

#include "realmmesh/service_host/layered_config_loader.hpp"
#include "realmmesh/service_host/service_host.hpp"
#include "realmmesh/service_host/startup_topology.hpp"

#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace realm::service_host {

/// 编排器:按拓扑波次构造 ServiceHost,entry 服务在全员 ready 后放行;
/// 关停按拓扑反序。
/// 控制器裁定:不另造 MeshServiceSpec,直接复用 startup_topology.hpp 的
/// ServiceSpec。
class MeshHost final {
public:
    /// 模式 2:拓扑收窄为仅 <name> 单服务(depends_on 清空);
    /// 名字未在 specs 声明时抛 std::invalid_argument。
    [[nodiscard]] static std::vector<ServiceSpec> narrow_single_service(
        std::vector<ServiceSpec> specs, std::string_view name);

    MeshHost(
        std::filesystem::path config_root,
        std::vector<ServiceSpec> specs,
        CliOverrides overrides = {});

    /// 波次启动;任一失败 → 已启动者反序回收后返回 false。
    /// 波内顺序构造:ServiceHost::start() 非阻塞(runtime 自持 IO 线程),
    /// 语义等同并行,不引入额外线程。
    /// 一次性语义:成功后二次调用会因端口重复 bind 失败并触发整体 shutdown。
    [[nodiscard]] bool start_all();

    /// entry 服务是否允许放行:specs 全部服务均已启动且 ready;
    /// 无 entry spec 时全员 ready 即为 true。
    [[nodiscard]] bool entry_ready() const noexcept;

    /// 每 tick 轮询全部 host 的发现续约。
    void tick();

    /// 反序关停全部服务;hosts 保留,service() 与指标查询仍可用。
    void shutdown();

    /// 单服务访问(模式 2 单服务时也用它);未知名字抛 std::out_of_range。
    [[nodiscard]] ServiceHost& service(std::string_view name);

private:
    std::filesystem::path config_root_;
    CliOverrides overrides_;
    StartupTopology topology_;
    std::map<std::string, std::unique_ptr<ServiceHost>> hosts_;
};

}  // namespace realm::service_host
