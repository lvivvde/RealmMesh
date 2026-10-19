#pragma once
#include <memory>
namespace realm::service_host {
class ServiceModule;
struct ServiceModuleContext;
}  // namespace realm::service_host
namespace realm::game::scene {
[[nodiscard]] std::unique_ptr<service_host::ServiceModule> make_scene_module(
    const service_host::ServiceModuleContext& context);
}
