#include "realmmesh/service_host/mesh_host.hpp"

#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/observability/logger.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace realm::service_host {
namespace {

[[nodiscard]] bool endpoint_is_reachable(
    const network::TransportEndpoint& endpoint) noexcept {
    // 探测用套接字在同一次调用内关闭。Linux 直接带 SOCK_CLOEXEC,堵住其他
    // 线程恰在此刻 fork+exec 时的泄漏窗口;macOS 没有该标志，窗口很短，不另处理。
#if defined(__linux__)
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
#else
    const int descriptor = ::socket(AF_INET, SOCK_STREAM, 0);
#endif
    if (descriptor < 0) return false;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(endpoint.port);
    std::string host = endpoint.address;
    if (host == "0.0.0.0") host = "127.0.0.1";
    const bool parsed =
        ::inet_pton(AF_INET, host.c_str(), &address.sin_addr) == 1;
    const bool connected =
        parsed && ::connect(
                      descriptor,
                      reinterpret_cast<const sockaddr*>(&address),
                      sizeof(address)) == 0;
    ::close(descriptor);
    return connected;
}

[[nodiscard]] std::optional<network::TransportEndpoint> tls_listener(
    const edge::EdgeConfig& config) {
    const auto found = std::ranges::find_if(
        config.transports, [](const network::TransportConfig& transport) {
            return transport.enabled &&
                   transport.protocol == network::TransportProtocol::TlsTcp &&
                   transport.listen_port != 0;
        });
    if (found == config.transports.end()) return std::nullopt;
    return network::TransportEndpoint{
        .name = found->name,
        .protocol = found->protocol,
        .address = found->listen_address,
        .port = found->listen_port,
    };
}

[[nodiscard]] bool has_tls_endpoint(
    const std::vector<cluster::ServiceInstance>& instances) {
    return std::ranges::any_of(instances, [](const auto& instance) {
        return std::ranges::any_of(
            instance.endpoints, [](const auto& endpoint) {
                return endpoint.protocol == network::TransportProtocol::TlsTcp;
            });
    });
}

}  // namespace

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
    ServiceModuleRegistry modules,
    CliOverrides overrides)
    : config_root_(std::move(config_root)),
      modules_(std::move(modules)),
      overrides_(std::move(overrides)),
      topology_(std::move(specs)) {}

bool MeshHost::start_all() {
    if (topology_.all_names().size() == 1) {
        const auto& name = topology_.all_names().front();
        const auto* registration = modules_.find(name);
        if (registration != nullptr &&
            registration->mode2_waits_for_dependency &&
            registration->dependency.has_value()) {
            try {
                if (!mode2_dependency_ready(name, *registration->dependency)) {
                    std::cerr << "realm_mesh: " << name
                              << " dependencies are not ready\n";
                    return false;
                }
            } catch (const std::exception& error) {
                std::cerr << "realm_mesh: " << name
                          << " dependency check failed: " << error.what()
                          << '\n';
                return false;
            }
        }
    }
    for (const auto& wave : topology_.waves()) {
        for (const auto& name : wave) {
            std::unique_ptr<ServiceHost> host;
            try {
                host = std::make_unique<ServiceHost>(
                    config_root_, name, modules_, overrides_);
                if (!host->start()) {
                    throw std::runtime_error(name + " not ready");
                }
                hosts_.emplace(name, std::move(host));
            } catch (const std::exception& error) {
                // 启动失败诊断不能丢:name + what() 经该服务自身 logger 落盘
                // (构造即失败无 logger 时退到 stderr);fail-fast 回收不变。
                if (host != nullptr) {
                    static_cast<void>(host->logger().error(
                        "service_start_failed",
                        name + " service failed",
                        {observability::field("error_message", error.what())}));
                } else {
                    std::cerr << "realm_mesh: " << name
                              << " failed to start: " << error.what() << '\n';
                }
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

bool MeshHost::mode2_dependency_ready(
    const std::string& name, const std::string& dependency) const {
    const auto self = LayeredConfigLoader::load(config_root_, name, overrides_);
    const auto& discovery = self.host().service_discovery;
    const auto deadline =
        std::chrono::steady_clock::now() + discovery.startup_timeout;
    if (discovery.enabled) {
        cluster::EtcdServiceRegistry registry(
            cluster::make_etcd_registry_options(discovery));
        do {
            if (has_tls_endpoint(registry.discover(dependency))) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    const auto dependency_config =
        LayeredConfigLoader::load(config_root_, dependency, overrides_);
    const auto endpoint = tls_listener(dependency_config.edge());
    if (!endpoint.has_value()) return false;
    do {
        if (endpoint_is_reachable(*endpoint)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

}  // namespace realm::service_host
