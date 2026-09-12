#include "realmmesh/game/login_verify/login_verify_config.hpp"

#include <cstdlib>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

namespace realm::game::login_verify {
namespace {

// 统一使用 sol::lua_nil 而非 sol::nil:sol2 在 macOS 上会因 nil 宏已定义
// (version.hpp)导致 sol::nil 在同一工程的不同翻译单元里时有时无。
// 与 gateway_config_loader.cpp 同一处理。

[[nodiscard]] std::string optional_string(
    const sol::table& table, std::string_view field, std::string fallback) {
    const sol::object value = table.raw_get<sol::object>(std::string(field));
    if (value == sol::lua_nil) return fallback;
    if (!value.is<std::string>()) {
        throw std::invalid_argument(
            "login_verify field " + std::string(field) + " must be a string");
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
            "login_verify field " + std::string(field) +
            " must be a port number");
    }
    return static_cast<std::uint16_t>(value.as<lua_Integer>());
}

/// TLS 路径:配置直填优先,否则按环境变量名解析(先例同 gateway 传输配置);
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
            "login_verify TLS identity requires " + std::string(path_field) +
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

LoginVerifyConfig LoginVerifyConfigLoader::parse(const sol::table& root) {
    const sol::object section = root.raw_get<sol::object>("login_verify");
    if (!section.is<sol::table>()) {
        throw std::invalid_argument(
            "login_verify configuration requires a login_verify table");
    }
    const sol::table table = section.as<sol::table>();

    LoginVerifyConfig config;
    config.listen_address =
        optional_string(table, "listen_address", config.listen_address);
    config.listen_port = optional_port(table, "listen_port", config.listen_port);
    config.kid = optional_string(table, "kid", config.kid);
    config.accounts_file = optional_string(
        table, "accounts_file", config.accounts_file.string());
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

}  // namespace realm::game::login_verify
