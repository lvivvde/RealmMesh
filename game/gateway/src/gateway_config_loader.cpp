#include "realmmesh/game/gateway/gateway_config_loader.hpp"

#include "realmmesh/scripting/lua_runtime.hpp"

#include <cstdlib>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace realm::game::gateway {
namespace {

[[nodiscard]] std::string required_string(
    const sol::table& table,
    std::string_view field) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (!value.is<std::string>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) + " must be a string");
    }
    return value.as<std::string>();
}

[[nodiscard]] std::string optional_string(
    const sol::table& table,
    std::string_view field,
    std::string fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::nil) return fallback;
    if (!value.is<std::string>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) + " must be a string");
    }
    return value.as<std::string>();
}

template <typename Integer>
[[nodiscard]] Integer optional_integer(
    const sol::table& table,
    std::string_view field,
    Integer fallback) {
    const sol::object object = table.raw_get<sol::object>(std::string(field));
    if (object == sol::nil) return fallback;
    if (!object.is<lua_Integer>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) + " must be an integer");
    }
    const lua_Integer value = object.as<lua_Integer>();
    if (value < 0 || static_cast<unsigned long long>(value) >
                         static_cast<unsigned long long>(
                             std::numeric_limits<Integer>::max())) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) + " is out of range");
    }
    return static_cast<Integer>(value);
}

[[nodiscard]] bool optional_boolean(
    const sol::table& table,
    std::string_view field,
    bool fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::nil) return fallback;
    if (!value.is<bool>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) + " must be boolean");
    }
    return value.as<bool>();
}

[[nodiscard]] network::TransportProtocol parse_protocol(std::string_view value) {
    if (value == "quic") return network::TransportProtocol::Quic;
    if (value == "tls_tcp") return network::TransportProtocol::TlsTcp;
    throw std::invalid_argument(
        "unsupported transport protocol: " + std::string(value));
}

[[nodiscard]] std::string path_from_config_or_environment(
    const sol::table& table,
    std::string_view path_field,
    std::string_view environment_field) {
    auto path = optional_string(table, path_field, "");
    if (!path.empty()) {
        return path;
    }
    const auto environment = optional_string(table, environment_field, "");
    if (environment.empty()) {
        throw std::invalid_argument(
            "enabled secure transport requires " + std::string(path_field) +
            " or " + std::string(environment_field));
    }
    const char* value = std::getenv(environment.c_str());
    if (value == nullptr || *value == '\0') {
        throw std::invalid_argument(
            "TLS path environment variable is not set: " + environment);
    }
    return value;
}

[[nodiscard]] network::TransportConfig read_transport(const sol::table& table) {
    network::TransportConfig config;
    config.name = required_string(table, "name");
    config.protocol = parse_protocol(required_string(table, "protocol"));
    config.enabled = optional_boolean(table, "enabled", config.enabled);
    config.listen_address = optional_string(
        table, "listen_address", std::move(config.listen_address));
    config.listen_port = optional_integer(
        table, "listen_port", config.listen_port);
    config.max_sessions = optional_integer(
        table, "max_sessions", config.max_sessions);
    config.max_payload_size = optional_integer(
        table, "max_payload_size", config.max_payload_size);
    config.max_pending_output_bytes = optional_integer(
        table,
        "max_pending_output_bytes",
        config.max_pending_output_bytes);
    config.idle_timeout = std::chrono::milliseconds(
        optional_integer<std::int64_t>(
            table, "idle_timeout_ms", config.idle_timeout.count()));
    config.handshake_timeout = std::chrono::milliseconds(
        optional_integer<std::int64_t>(
            table,
            "handshake_timeout_ms",
            config.handshake_timeout.count()));

    if (config.enabled) {
        config.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = path_from_config_or_environment(
                table,
                "certificate_chain_file",
                "certificate_chain_file_environment"),
            .private_key_file = path_from_config_or_environment(
                table,
                "private_key_file",
                "private_key_file_environment"),
            .alpn = optional_string(table, "alpn", "realmmesh-edge/1"),
        };
    }
    return config;
}

}  // namespace

GatewayConfig GatewayConfigLoader::load(const std::filesystem::path& path) {
    scripting::LuaRuntime runtime;
    std::string error;
    if (!runtime.load_module("gateway_config", path, &error)) {
        throw std::runtime_error(
            "failed to load gateway configuration: " + error);
    }
    const sol::table root = runtime.module("gateway_config");
    GatewayConfig config;
    config.tick_rate = optional_integer(root, "tick_rate", config.tick_rate);
    config.max_events_per_frame = optional_integer(
        root, "max_events_per_frame", config.max_events_per_frame);
    config.downstream_address = optional_string(
        root,
        "downstream_address",
        std::move(config.downstream_address));
    config.downstream_port = optional_integer(
        root, "downstream_port", config.downstream_port);

    const sol::object runtime_value = root.raw_get<sol::object>("runtime");
    if (runtime_value != sol::nil) {
        if (!runtime_value.is<sol::table>()) {
            throw std::invalid_argument("gateway config runtime must be a table");
        }
        const sol::table runtime_table = runtime_value.as<sol::table>();
        config.runtime.inbound_capacity = optional_integer(
            runtime_table,
            "inbound_capacity",
            config.runtime.inbound_capacity);
        config.runtime.outbound_capacity = optional_integer(
            runtime_table,
            "outbound_capacity",
            config.runtime.outbound_capacity);
        config.runtime.max_commands_per_cycle = optional_integer(
            runtime_table,
            "max_commands_per_cycle",
            config.runtime.max_commands_per_cycle);
        config.runtime.io_poll_interval = std::chrono::milliseconds(
            optional_integer<std::int64_t>(
                runtime_table,
                "io_poll_interval_ms",
                config.runtime.io_poll_interval.count()));
    }

    const sol::object discovery_value =
        root.raw_get<sol::object>("service_discovery");
    if (discovery_value != sol::nil) {
        if (!discovery_value.is<sol::table>()) {
            throw std::invalid_argument(
                "gateway config service_discovery must be a table");
        }
        const sol::table discovery = discovery_value.as<sol::table>();
        config.service_discovery.enabled = optional_boolean(
            discovery, "enabled", config.service_discovery.enabled);
        config.service_discovery.required = optional_boolean(
            discovery, "required", config.service_discovery.required);
        config.service_discovery.endpoint = optional_string(
            discovery, "endpoint", std::move(config.service_discovery.endpoint));
        config.service_discovery.key_prefix = optional_string(
            discovery,
            "key_prefix",
            std::move(config.service_discovery.key_prefix));
        config.service_discovery.instance_id = optional_string(
            discovery,
            "instance_id",
            std::move(config.service_discovery.instance_id));
        config.service_discovery.node_id = optional_string(
            discovery, "node_id", std::move(config.service_discovery.node_id));
        config.service_discovery.zone = optional_string(
            discovery, "zone", std::move(config.service_discovery.zone));
        config.service_discovery.advertise_address = optional_string(
            discovery,
            "advertise_address",
            std::move(config.service_discovery.advertise_address));
        config.service_discovery.lease_ttl = std::chrono::seconds(
            optional_integer<std::int64_t>(
                discovery,
                "lease_ttl_seconds",
                config.service_discovery.lease_ttl.count()));
        config.service_discovery.request_timeout = std::chrono::milliseconds(
            optional_integer<std::int64_t>(
                discovery,
                "request_timeout_ms",
                config.service_discovery.request_timeout.count()));
        config.service_discovery.watch_interval = std::chrono::milliseconds(
            optional_integer<std::int64_t>(
                discovery,
                "watch_interval_ms",
                config.service_discovery.watch_interval.count()));
    }
    if (config.tick_rate == 0 || config.max_events_per_frame == 0) {
        throw std::invalid_argument(
            "gateway tick rate and max events per frame must be positive");
    }
    if (config.service_discovery.enabled &&
        (config.service_discovery.endpoint.empty() ||
         config.service_discovery.key_prefix.empty() ||
         config.service_discovery.instance_id.empty() ||
         config.service_discovery.node_id.empty() ||
         config.service_discovery.zone.empty() ||
         config.service_discovery.advertise_address.empty() ||
         config.service_discovery.lease_ttl <= std::chrono::seconds::zero() ||
         config.service_discovery.request_timeout <=
             std::chrono::milliseconds::zero() ||
         config.service_discovery.watch_interval <=
             std::chrono::milliseconds::zero())) {
        throw std::invalid_argument(
            "enabled service discovery configuration is incomplete");
    }

    const sol::object transports_value = root.raw_get<sol::object>("transports");
    if (!transports_value.is<sol::table>()) {
        throw std::invalid_argument("gateway config transports must be a table");
    }
    const sol::table transports = transports_value.as<sol::table>();
    config.transports.clear();
    config.transports.reserve(transports.size());
    for (std::size_t index = 1; index <= transports.size(); ++index) {
        const sol::object transport_value =
            transports.raw_get<sol::object>(index);
        if (!transport_value.is<sol::table>()) {
            throw std::invalid_argument(
                "each gateway transport must be a table");
        }
        config.transports.push_back(
            read_transport(transport_value.as<sol::table>()));
    }
    return config;
}

}  // namespace realm::game::gateway
