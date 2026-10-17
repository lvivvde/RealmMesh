#pragma once

// 宿主测试的模块注册表:生产服务清单(gateway/realm/login_verify/queue)
// 之外,按名追加只持 EdgeRuntime、帧循环空转的接入面模块,供不关心业务
// 体的宿主与编排用例使用。
#include "realmmesh/edge/edge_runtime.hpp"
#include "realmmesh/mesh_host/service_modules.hpp"
#include "realmmesh/service_host/edge_service_module.hpp"
#include "realmmesh/service_host/service_host.hpp"

#include <initializer_list>
#include <memory>
#include <string>
#include <string_view>

namespace realm::service_host::test_modules {

[[nodiscard]] inline ServiceModuleRegistry test_service_modules(
    std::initializer_list<std::string_view> edge_names = {}) {
    auto modules = mesh_host::game_service_modules();
    for (const auto name : edge_names) {
        modules.add(
            std::string(name),
            {.factory = [](const ServiceModuleContext& context) {
                return std::make_unique<EdgeServiceModule>(context);
            }});
    }
    return modules;
}

/// 接入面模块的 EdgeRuntime;模块不是 EdgeServiceModule 时抛 std::bad_cast。
[[nodiscard]] inline edge::EdgeRuntime& edge_runtime(ServiceHost& host) {
    return dynamic_cast<EdgeServiceModule&>(host.module()).runtime();
}

}  // namespace realm::service_host::test_modules
