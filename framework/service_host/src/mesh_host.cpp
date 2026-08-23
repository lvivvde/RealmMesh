#include "realmmesh/service_host/mesh_host.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <utility>

namespace realm::service_host {

std::vector<ServiceSpec> MeshHost::narrow_single_service(
    std::vector<ServiceSpec> specs, std::string_view name) {
    const auto match = std::find_if(
        specs.begin(), specs.end(), [&name](const ServiceSpec& spec) {
            return spec.name == name;
        });
    if (match == specs.end()) {
        throw std::invalid_argument(
            "service is not declared in main.config: " + std::string(name));
    }
    ServiceSpec single = std::move(*match);
    single.depends_on.clear();
    return {std::move(single)};
}

MeshHost::MeshHost(
    std::filesystem::path config_root,
    std::vector<ServiceSpec> specs,
    CliOverrides overrides)
    : config_root_(std::move(config_root)),
      overrides_(std::move(overrides)),
      topology_(std::move(specs)) {}

bool MeshHost::start_all() {
    for (const auto& wave : topology_.waves()) {
        for (const auto& name : wave) {
            try {
                auto host = std::make_unique<ServiceHost>(
                    config_root_, name, overrides_);
                if (!host->start()) {
                    throw std::runtime_error(name + " not ready");
                }
                hosts_.emplace(name, std::move(host));
            } catch (const std::exception&) {
                shutdown();  // 反序回收已启动者
                return false;
            }
        }
    }
    return entry_ready();
}

bool MeshHost::entry_ready() const noexcept {
    // 放行条件:拓扑内每个服务都有已启动且 ready 的 host;
    // 无 entry spec 时该条件同样成立(全员 ready 即 true)。
    for (const auto& name : topology_.all_names()) {
        const auto host = hosts_.find(name);
        if (host == hosts_.end() || !host->second->ready()) {
            return false;
        }
    }
    return true;
}

void MeshHost::tick() {
    for (auto& [name, host] : hosts_) {
        host->tick();
    }
}

void MeshHost::shutdown() {
    for (const auto& name : topology_.shutdown_order()) {
        const auto host = hosts_.find(name);
        if (host != hosts_.end()) {
            host->second->stop();
        }
    }
}

ServiceHost& MeshHost::service(std::string_view name) {
    const auto host = hosts_.find(std::string(name));
    if (host == hosts_.end()) {
        throw std::out_of_range("unknown service: " + std::string(name));
    }
    return *host->second;
}

}  // namespace realm::service_host
