#include "realmmesh/mesh_host/service_modules.hpp"

#include "realmmesh/game/login_verify/login_verify_module.hpp"
#include "realmmesh/game/queue/queue_module.hpp"
#include "realmmesh/service_host/frame_service_modules.hpp"

namespace realm::mesh_host {

service_host::ServiceModuleRegistry game_service_modules() {
    service_host::ServiceModuleRegistry modules;
    // #45:handoff 直连端点来自 Realm 发现,缺失退回静态下游。
    modules.add(
        "gateway",
        {.factory = service_host::make_gateway_module,
         .dependency = "realm",
         .mode2_waits_for_dependency = true});
    modules.add(
        "realm",
        {.factory = service_host::make_realm_module, .dependency = "gateway"});
    // HTTPS 服务无下游依赖:状态经 etcd 而非服务依赖。
    modules.add(
        "login_verify",
        {.factory = game::login_verify::make_login_verify_module});
    modules.add("queue", {.factory = game::queue::make_queue_module});
    return modules;
}

}  // namespace realm::mesh_host
