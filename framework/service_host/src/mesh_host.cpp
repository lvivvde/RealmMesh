#include "realmmesh/service_host/mesh_host.hpp"

#include "realmmesh/cluster/etcd_service_registry.hpp"
#include "realmmesh/cluster/service_bootstrap.hpp"
#include "realmmesh/game/gateway/gateway_server.hpp"
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
    const int descriptor = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
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
    const game::gateway::GatewayConfig& config) {
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
    CliOverrides overrides)
    : config_root_(std::move(config_root)),
      overrides_(std::move(overrides)),
      topology_(std::move(specs)) {}

bool MeshHost::start_all() {
    if (topology_.all_names().size() == 1 &&
        topology_.all_names().front() == "gateway") {
        try {
            if (!mode2_gateway_dependencies_ready()) {
                std::cerr << "realm_mesh: gateway dependencies are not ready\n";
                return false;
            }
        } catch (const std::exception& error) {
            std::cerr << "realm_mesh: gateway dependency check failed: "
                      << error.what() << '\n';
            return false;
        }
    }
    for (const auto& wave : topology_.waves()) {
        for (const auto& name : wave) {
            std::unique_ptr<ServiceHost> host;
            try {
                host = std::make_unique<ServiceHost>(
                    config_root_, name, overrides_);
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

bool MeshHost::mode2_gateway_dependencies_ready() const {
    const auto gateway =
        LayeredConfigLoader::load(config_root_, "gateway", overrides_);
    const auto deadline = std::chrono::steady_clock::now() +
                          gateway.service_discovery.startup_timeout;
    if (gateway.service_discovery.enabled) {
        cluster::EtcdServiceRegistry registry(
            cluster::make_etcd_registry_options(gateway.service_discovery));
        do {
            if (has_tls_endpoint(
                    registry.discover(cluster::ServiceType::Login)) &&
                has_tls_endpoint(
                    registry.discover(cluster::ServiceType::Realm))) {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        } while (std::chrono::steady_clock::now() < deadline);
        return false;
    }

    std::vector<network::TransportEndpoint> endpoints;
    for (const std::string_view service : {"login", "realm"}) {
        const auto config =
            LayeredConfigLoader::load(config_root_, service, overrides_);
        const auto endpoint = tls_listener(config);
        if (!endpoint.has_value()) return false;
        endpoints.push_back(*endpoint);
    }
    do {
        if (std::ranges::all_of(endpoints, endpoint_is_reachable)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}

}  // namespace realm::service_host
