#include "realmmesh/service_host/service_module.hpp"

#include "realmmesh/cluster/service_registry.hpp"
#include "realmmesh/edge/edge_runtime.hpp"
#include "realmmesh/service_host/edge_service_module.hpp"

#include <stdexcept>
#include <string>
#include <utility>

namespace realm::service_host {

void ServiceModuleRegistry::add(
    std::string name, ServiceModuleRegistration registration) {
    if (!cluster::is_valid_service_type(name)) {
        throw std::invalid_argument("invalid service module name: " + name);
    }
    if (!registration.factory) {
        throw std::invalid_argument(
            "service module factory is required: " + name);
    }
    if (registration.dependency.has_value() &&
        !cluster::is_valid_service_type(*registration.dependency)) {
        throw std::invalid_argument(
            "invalid service module dependency: " + *registration.dependency);
    }
    const auto [position, inserted] =
        modules_.emplace(std::move(name), std::move(registration));
    if (!inserted) {
        throw std::invalid_argument(
            "service module already registered: " + position->first);
    }
}

const ServiceModuleRegistration* ServiceModuleRegistry::find(
    std::string_view name) const noexcept {
    const auto position = modules_.find(name);
    return position == modules_.end() ? nullptr : &position->second;
}

EdgeServiceModule::EdgeServiceModule(const ServiceModuleContext& context)
    : logger_(&context.logger),
      service_name_(context.config.service_name()) {
    const auto& edge = context.config.edge();
    runtime_ = std::make_unique<edge::EdgeRuntime>(
        edge.transports, edge.runtime, edge.ingress_source, logger_);
}

EdgeServiceModule::~EdgeServiceModule() = default;

void EdgeServiceModule::start() { runtime_->start(); }

void EdgeServiceModule::stop() { runtime_->stop(); }

bool EdgeServiceModule::running() const noexcept {
    return runtime_->running();
}

std::span<const network::TransportEndpoint>
EdgeServiceModule::local_endpoints() const noexcept {
    return runtime_->local_endpoints();
}

void EdgeServiceModule::tick(const ServiceTickContext&) {}

std::optional<std::string> EdgeServiceModule::terminal_error() const {
    return runtime_->terminal_error();
}

edge::EdgeRuntime& EdgeServiceModule::runtime() noexcept { return *runtime_; }

const edge::EdgeRuntime& EdgeServiceModule::runtime() const noexcept {
    return *runtime_;
}

observability::Logger& EdgeServiceModule::logger() const noexcept {
    return *logger_;
}

const std::string& EdgeServiceModule::service_name() const noexcept {
    return service_name_;
}

}  // namespace realm::service_host
