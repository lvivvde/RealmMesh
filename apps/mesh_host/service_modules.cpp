#include "realmmesh/mesh_host/service_modules.hpp"

#include "realmmesh/game/gateway/gateway_module.hpp"
#include "realmmesh/game/login_verify/login_verify_module.hpp"
#include "realmmesh/game/queue/queue_module.hpp"
#include "realmmesh/game/realm/realm_module.hpp"

namespace realm::mesh_host {

service_host::ServiceModuleRegistry game_service_modules() {
    service_host::ServiceModuleRegistry modules;
    // #45:handoff 直连端点来自 Realm 发现,缺失退回静态下游。
    modules.add(
        "gateway",
        {.factory = game::gateway::make_gateway_module,
         .dependency = "realm",
         .mode2_waits_for_dependency = true});
    // Realm 与 HTTPS 服务都不解析其他服务的端点:状态经 etcd 而非服务依赖。
    modules.add("realm", {.factory = game::realm::make_realm_module});
    modules.add(
        "login_verify",
        {.factory = game::login_verify::make_login_verify_module});
    modules.add("queue", {.factory = game::queue::make_queue_module});
    return modules;
}

}  // namespace realm::mesh_host
