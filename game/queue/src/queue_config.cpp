#include "realmmesh/game/queue/queue_config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace realm::game::queue {
namespace {

[[nodiscard]] std::string optional_string(
    const sol::table& table, std::string_view field, std::string fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<std::string>()) {
        throw std::invalid_argument(
            "queue field " + std::string(field) + " must be a string");
    }
    return value.as<std::string>();
}

[[nodiscard]] std::uint16_t optional_port(
    const sol::table& table, std::string_view field, std::uint16_t fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<lua_Integer>() || value.as<lua_Integer>() < 0 ||
        value.as<lua_Integer>() > 65535) {
        throw std::invalid_argument(
            "queue field " + std::string(field) + " must be a port number");
    }
    return static_cast<std::uint16_t>(value.as<lua_Integer>());
}

/// 非负整数字段(时长秒数/步长/容量);显式负数或非整即抛。
[[nodiscard]] std::uint64_t optional_positive(
    const sol::table& table, std::string_view field, std::uint64_t fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<lua_Integer>() || value.as<lua_Integer>() < 0) {
        throw std::invalid_argument(
            "queue field " + std::string(field) + " must be a non-negative number");
    }
    return static_cast<std::uint64_t>(value.as<lua_Integer>());
}

/// TLS 路径:配置直填优先,否则按环境变量名解析(先例同 login_verify);
/// 两路皆空抛 std::invalid_argument。
[[nodiscard]] std::string path_from_config_or_environment(
    const sol::table& table,
    std::string_view path_field,
    std::string_view environment_field) {
    const std::string path = optional_string(table, path_field, "");
    if (!path.empty()) return path;
    const std::string environment =
        optional_string(table, environment_field, "");
    if (environment.empty()) {
        throw std::invalid_argument(
            "queue TLS identity requires " + std::string(path_field) +
            " or " + std::string(environment_field));
    }
    const char* value = std::getenv(environment.c_str());
    if (value == nullptr || *value == '\0') {
        throw std::invalid_argument(
            "TLS path environment variable is not set: " + environment);
    }
    return value;
}

}  // namespace

QueueConfig QueueConfigLoader::parse(const sol::table& root) {
    const sol::object section = root.raw_get<sol::object>("queue");
    if (!section.is<sol::table>()) {
        throw std::invalid_argument(
            "queue configuration requires a queue table");
    }
    const sol::table table = section.as<sol::table>();

    QueueConfig config;
    config.listen_address =
        optional_string(table, "listen_address", config.listen_address);
    config.listen_port = optional_port(table, "listen_port", config.listen_port);
    config.kid = optional_string(table, "kid", config.kid);
    config.identity_kid =
        optional_string(table, "identity_kid", config.identity_kid);
    config.identity_issuer =
        optional_string(table, "identity_issuer", config.identity_issuer);
    config.release_step =
        optional_positive(table, "release_step", config.release_step);
    config.release_interval = std::chrono::milliseconds(optional_positive(
        table,
        "release_interval_seconds",
        static_cast<std::uint64_t>(config.release_interval.count()) / 1000U) *
        1000U);
    config.budget_interval = std::chrono::milliseconds(optional_positive(
        table,
        "budget_interval_seconds",
        static_cast<std::uint64_t>(config.budget_interval.count()) / 1000U) *
        1000U);
    config.queued_ticket_ttl = std::chrono::seconds(optional_positive(
        table,
        "queued_ticket_ttl_seconds",
        static_cast<std::uint64_t>(config.queued_ticket_ttl.count())));
    config.admit_grace = std::chrono::seconds(optional_positive(
        table,
        "admit_grace_seconds",
        static_cast<std::uint64_t>(config.admit_grace.count())));
    config.idempotency_ttl = std::chrono::seconds(optional_positive(
        table,
        "idempotency_ttl_seconds",
        static_cast<std::uint64_t>(config.idempotency_ttl.count())));
    config.idempotency_capacity = static_cast<std::size_t>(optional_positive(
        table, "idempotency_capacity", config.idempotency_capacity));
    config.budget_prefix =
        optional_string(table, "budget_prefix", config.budget_prefix);
    config.snapshot_key =
        optional_string(table, "snapshot_key", config.snapshot_key);
    config.etcd_endpoint =
        optional_string(table, "etcd_endpoint", config.etcd_endpoint);
    config.tls = network::TransportConfig::TlsServerIdentity{
        .certificate_chain_file = path_from_config_or_environment(
            table,
            "certificate_chain_file",
            "certificate_chain_file_environment"),
        .private_key_file = path_from_config_or_environment(
            table, "private_key_file", "private_key_file_environment"),
        .alpn = optional_string(table, "alpn", "http/1.1"),
    };
    return config;
}

}  // namespace realm::game::queue
