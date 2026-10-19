#include "service_config_lua.hpp"

#include "realmmesh/network/transport/transport_config.hpp"

#include <sol/sol.hpp>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 统一使用 sol::lua_nil 而非 sol::nil:sol2 在 macOS 上会因 nil 宏已定义
// (Objective-C)或 __MAC_OS_X_VERSION_MAX_ALLOWED 而把 SOL_NIL 置为关闭
// (version.hpp),导致 sol::nil 在同一工程的不同翻译单元里时有时无。
// 两者是同一个 lua_nil_t 值。

namespace realm::service_host {
namespace {

[[nodiscard]] std::string required_string(
    const sol::table& table, std::string_view field) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (!value.is<std::string>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) + " must be a string");
    }
    return value.as<std::string>();
}

[[nodiscard]] std::string optional_string(
    const sol::table& table, std::string_view field, std::string fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<std::string>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) + " must be a string");
    }
    return value.as<std::string>();
}

template <typename Integer>
[[nodiscard]] Integer optional_integer(
    const sol::table& table, std::string_view field, Integer fallback) {
    const sol::object object = table.raw_get<sol::object>(std::string(field));
    if (object == sol::lua_nil) return fallback;
    if (!object.is<lua_Integer>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) +
            " must be an integer");
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
    const sol::table& table, std::string_view field, bool fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<bool>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) + " must be boolean");
    }
    return value.as<bool>();
}

[[nodiscard]] std::vector<std::string> optional_string_array(
    const sol::table& table, std::string_view field) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return {};
    if (!value.is<sol::table>()) {
        throw std::invalid_argument(
            "configuration field " + std::string(field) +
            " must be a table");
    }
    const auto values = value.as<sol::table>();
    std::vector<std::string> result;
    result.reserve(values.size());
    for (std::size_t index = 1; index <= values.size(); ++index) {
        const auto entry = values.raw_get<sol::object>(index);
        if (!entry.is<std::string>()) {
            throw std::invalid_argument(
                "configuration field " + std::string(field) +
                " must contain only strings");
        }
        result.push_back(entry.as<std::string>());
    }
    return result;
}

[[nodiscard]] network::TransportProtocol parse_protocol(
    std::string_view value) {
    if (value == "quic") return network::TransportProtocol::Quic;
    if (value == "tls_tcp") return network::TransportProtocol::TlsTcp;
    throw std::invalid_argument(
        "unsupported transport protocol: " + std::string(value));
}

[[nodiscard]] observability::Severity parse_severity(std::string_view value) {
    if (value == "trace" || value == "TRACE")
        return observability::Severity::Trace;
    if (value == "debug" || value == "DEBUG")
        return observability::Severity::Debug;
    if (value == "info" || value == "INFO")
        return observability::Severity::Info;
    if (value == "warn" || value == "WARN")
        return observability::Severity::Warn;
    if (value == "error" || value == "ERROR")
        return observability::Severity::Error;
    if (value == "fatal" || value == "FATAL")
        return observability::Severity::Fatal;
    throw std::invalid_argument(
        "unsupported logging level: " + std::string(value));
}

void read_logging_runtime_policy(
    const sol::table& logging, observability::LoggerConfig& config) {
    const sol::object module_levels_value =
        logging.raw_get<sol::object>("module_levels");
    if (module_levels_value != sol::lua_nil) {
        if (!module_levels_value.is<sol::table>()) {
            throw std::invalid_argument(
                "logging module_levels must be a table");
        }
        for (const auto& [key, value] : module_levels_value.as<sol::table>()) {
            if (!key.is<std::string>() || !value.is<std::string>()) {
                throw std::invalid_argument(
                    "logging module_levels entries must be string pairs");
            }
            config.module_levels.insert_or_assign(
                key.as<std::string>(), parse_severity(value.as<std::string>()));
        }
    }

    const sol::object sample_rates_value =
        logging.raw_get<sol::object>("sample_rates");
    if (sample_rates_value == sol::lua_nil) return;
    if (!sample_rates_value.is<sol::table>()) {
        throw std::invalid_argument("logging sample_rates must be a table");
    }
    for (const auto& [key, value] : sample_rates_value.as<sol::table>()) {
        if (!key.is<std::string>() || !value.is<double>()) {
            throw std::invalid_argument(
                "logging sample_rates entries must map strings to numbers");
        }
        const auto rate = value.as<double>();
        if (rate < 0.0 || rate > 1.0) {
            throw std::invalid_argument(
                "logging sample rates must be in [0, 1]");
        }
        config.sample_rates.insert_or_assign(key.as<std::string>(), rate);
    }
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
    config.listen_port =
        optional_integer(table, "listen_port", config.listen_port);
    config.max_sessions =
        optional_integer(table, "max_sessions", config.max_sessions);
    config.max_payload_size =
        optional_integer(table, "max_payload_size", config.max_payload_size);
    config.max_pending_output_bytes = optional_integer(
        table, "max_pending_output_bytes", config.max_pending_output_bytes);
    config.idle_timeout =
        std::chrono::milliseconds(optional_integer<std::int64_t>(
            table, "idle_timeout_ms", config.idle_timeout.count()));
    config.handshake_timeout =
        std::chrono::milliseconds(optional_integer<std::int64_t>(
            table, "handshake_timeout_ms", config.handshake_timeout.count()));

    if (config.enabled) {
        config.tls = network::TransportConfig::TlsServerIdentity{
            .certificate_chain_file = path_from_config_or_environment(
                table,
                "certificate_chain_file",
                "certificate_chain_file_environment"),
            .private_key_file = path_from_config_or_environment(
                table, "private_key_file", "private_key_file_environment"),
            .alpn = optional_string(table, "alpn", std::string{network::kEdgeAlpn}),
        };
        const auto client_ca = optional_string(table, "client_ca_file", "");
        const auto client_ca_env =
            optional_string(table, "client_ca_file_environment", "");
        if (!client_ca.empty() || !client_ca_env.empty()) {
            config.tls->client_ca_file = path_from_config_or_environment(
                table, "client_ca_file", "client_ca_file_environment");
            const sol::object allowed =
                table.raw_get<sol::object>("allowed_client_identities");
            if (!allowed.is<sol::table>()) {
                throw std::invalid_argument(
                    "mTLS requires allowed_client_identities");
            }
            for (const auto& item : allowed.as<sol::table>()) {
                if (!item.second.is<std::string>() ||
                    item.second.as<std::string>().empty()) {
                    throw std::invalid_argument("invalid mTLS client identity");
                }
                config.tls->allowed_client_identities.push_back(
                    item.second.as<std::string>());
            }
            if (config.tls->allowed_client_identities.empty()) {
                throw std::invalid_argument("empty mTLS client allowlist");
            }
            if (config.protocol != network::TransportProtocol::TlsTcp) {
                throw std::invalid_argument(
                    "client CA currently requires TLS/TCP");
            }
        }
    }
    return config;
}

/// 可选的表节段:缺失返回 nullopt,非表抛 invalid_argument。
[[nodiscard]] std::optional<sol::table> optional_section(
    const sol::table& root, std::string_view field) {
    const sol::object value = root.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return std::nullopt;
    if (!value.is<sol::table>()) {
        throw std::invalid_argument(
            "service config " + std::string(field) + " must be a table");
    }
    return value.as<sol::table>();
}

}  // namespace

HostConfig parse_host_config(const sol::table& root) {
    HostConfig config;
    if (const auto section = optional_section(root, "service_discovery")) {
        const sol::table& discovery = *section;
        config.service_discovery.enabled = optional_boolean(
            discovery, "enabled", config.service_discovery.enabled);
        config.service_discovery.required = optional_boolean(
            discovery, "required", config.service_discovery.required);
        config.service_discovery.endpoint = optional_string(
            discovery,
            "endpoint",
            std::move(config.service_discovery.endpoint));
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
        config.service_discovery.lease_ttl =
            std::chrono::seconds(optional_integer<std::int64_t>(
                discovery,
                "lease_ttl_seconds",
                config.service_discovery.lease_ttl.count()));
        config.service_discovery.request_timeout =
            std::chrono::milliseconds(optional_integer<std::int64_t>(
                discovery,
                "request_timeout_ms",
                config.service_discovery.request_timeout.count()));
        config.service_discovery.watch_interval =
            std::chrono::milliseconds(optional_integer<std::int64_t>(
                discovery,
                "watch_interval_ms",
                config.service_discovery.watch_interval.count()));
        config.service_discovery.startup_timeout =
            std::chrono::milliseconds(optional_integer<std::int64_t>(
                discovery,
                "startup_timeout_ms",
                config.service_discovery.startup_timeout.count()));
    }

    if (const auto section = optional_section(root, "logging")) {
        const sol::table& logging = *section;
        config.logging.min_severity = parse_severity(optional_string(
            logging,
            "level",
            std::string(
                observability::to_string(config.logging.min_severity))));
        config.logging.file_path = optional_string(
            logging, "file_path", config.logging.file_path.string());
        config.logging.normal_queue_capacity = optional_integer(
            logging,
            "normal_queue_capacity",
            config.logging.normal_queue_capacity);
        config.logging.priority_queue_capacity = optional_integer(
            logging,
            "priority_queue_capacity",
            config.logging.priority_queue_capacity);
        config.logging.max_file_size = optional_integer(
            logging, "file_size_bytes", config.logging.max_file_size);
        config.logging.retained_files = optional_integer(
            logging, "retained_files", config.logging.retained_files);
        config.logging.console =
            optional_boolean(logging, "console", config.logging.console);
        read_logging_runtime_policy(logging, config.logging);
        config.logging_metrics.listen_address = optional_string(
            logging,
            "metrics_listen_address",
            std::move(config.logging_metrics.listen_address));
        config.logging_metrics.port = optional_integer(
            logging, "metrics_port", config.logging_metrics.port);
        config.logging_identity.environment = optional_string(
            logging,
            "environment",
            std::move(config.logging_identity.environment));
        config.logging_identity.cluster = optional_string(
            logging, "cluster", std::move(config.logging_identity.cluster));
        config.logging_identity.region = optional_string(
            logging, "region", std::move(config.logging_identity.region));
        config.logging_identity.service_name =
            required_string(logging, "service_name");
    }
    config.logging_identity.service_instance =
        config.service_discovery.instance_id;
    config.logging_identity.node_id = config.service_discovery.node_id;
    config.logging_identity.zone = config.service_discovery.zone;
    if (config.logging.file_path.empty() ||
        config.logging_identity.service_name.empty() ||
        config.logging.normal_queue_capacity == 0 ||
        config.logging.priority_queue_capacity == 0 ||
        config.logging.max_file_size == 0 ||
        config.logging.retained_files == 0) {
        throw std::invalid_argument("logging configuration is incomplete");
    }
    if (config.logging_metrics.port != 0 &&
        config.logging_metrics.listen_address.empty()) {
        throw std::invalid_argument(
            "logging metrics listen address cannot be empty");
    }
    if (config.service_discovery.startup_timeout <=
        std::chrono::milliseconds::zero()) {
        throw std::invalid_argument(
            "service discovery startup timeout must be positive");
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
    return config;
}

edge::EdgeConfig parse_edge_config(const sol::table& root) {
    edge::EdgeConfig config;
    if (const auto section = optional_section(root, "ingress_source")) {
        const sol::table& source = *section;
        config.ingress_source.mode = edge::parse_edge_source_mode(
            optional_string(source, "mode", "direct_peer"));
        config.ingress_source.trusted_proxy_cidrs =
            optional_string_array(source, "trusted_proxy_cidrs");
    }
    config.ingress_source.validate();

    if (const auto section = optional_section(root, "runtime")) {
        const sol::table& runtime_table = *section;
        config.runtime.inbound_capacity = optional_integer(
            runtime_table, "inbound_capacity", config.runtime.inbound_capacity);
        config.runtime.outbound_capacity = optional_integer(
            runtime_table,
            "outbound_capacity",
            config.runtime.outbound_capacity);
        config.runtime.max_commands_per_cycle = optional_integer(
            runtime_table,
            "max_commands_per_cycle",
            config.runtime.max_commands_per_cycle);
        config.runtime.io_poll_interval =
            std::chrono::milliseconds(optional_integer<std::int64_t>(
                runtime_table,
                "io_poll_interval_ms",
                config.runtime.io_poll_interval.count()));
    }

    const sol::object transports_value =
        root.raw_get<sol::object>("transports");
    if (!transports_value.is<sol::table>()) {
        throw std::invalid_argument(
            "service config transports must be a table");
    }
    const sol::table transports = transports_value.as<sol::table>();
    config.transports.reserve(transports.size());
    for (std::size_t index = 1; index <= transports.size(); ++index) {
        const sol::object transport_value =
            transports.raw_get<sol::object>(index);
        if (!transport_value.is<sol::table>()) {
            throw std::invalid_argument("each transport must be a table");
        }
        config.transports.push_back(
            read_transport(transport_value.as<sol::table>()));
    }
    return config;
}

}  // namespace realm::service_host
