#include "realmmesh/game/gateway/gateway_config_lua.hpp"

#include "realmmesh/game/common/player_data_config_lua.hpp"

#include <sol/sol.hpp>

#include <chrono>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// 统一使用 sol::lua_nil 而非 sol::nil:sol2 在 macOS 上会因 nil 宏已定义
// (Objective-C)或 __MAC_OS_X_VERSION_MAX_ALLOWED 而把 SOL_NIL 置为关闭
// (version.hpp),导致 sol::nil 在同一工程的不同翻译单元里时有时无。
// 两者是同一个 lua_nil_t 值。

namespace realm::game::gateway {
namespace {

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

}  // namespace

GatewayConfig parse_gateway_config(const sol::table& root) {
    GatewayConfig config;
    config.tick_rate = optional_integer(root, "tick_rate", config.tick_rate);
    config.max_events_per_frame = optional_integer(
        root, "max_events_per_frame", config.max_events_per_frame);
    config.downstream_address = optional_string(
        root, "downstream_address", std::move(config.downstream_address));
    config.downstream_port =
        optional_integer(root, "downstream_port", config.downstream_port);
    config.login.fetch_capacity = optional_integer(
        root, "pipeline_fetch_capacity", config.login.fetch_capacity);
    config.login.fetch_workers = optional_integer(
        root, "fetch_workers", config.login.fetch_workers);
    config.login.fetch_timeout = std::chrono::milliseconds{
        optional_integer<std::int64_t>(
            root,
            "fetch_timeout_ms",
            config.login.fetch_timeout.count())};
    config.login.fetch_retry_base = std::chrono::milliseconds{
        optional_integer<std::int64_t>(
            root,
            "fetch_retry_base_ms",
            config.login.fetch_retry_base.count())};
    config.login.fetch_retry_max =
        static_cast<unsigned>(optional_integer<unsigned>(
            root, "fetch_retry_max", config.login.fetch_retry_max));
    config.login.handoff_grace = std::chrono::milliseconds{
        optional_integer<std::int64_t>(
            root,
            "handoff_grace_ms",
            config.login.handoff_grace.count())};

    config.player_data = common::parse_player_data_config(root);

    const sol::object credential_value =
        root.raw_get<sol::object>("credential_ingress");
    if (credential_value != sol::lua_nil) {
        if (!credential_value.is<sol::table>()) {
            throw std::invalid_argument(
                "gateway config credential_ingress must be a table");
        }
        const auto ingress = credential_value.as<sol::table>();
        auto& security = config.login.credential_ingress;
        security.max_attach_envelope_bytes = optional_integer(
            ingress,
            "max_attach_envelope_bytes",
            security.max_attach_envelope_bytes);
        security.max_identity_token_bytes = optional_integer(
            ingress,
            "max_identity_token_bytes",
            security.max_identity_token_bytes);
        security.max_admission_grant_bytes = optional_integer(
            ingress,
            "max_admission_grant_bytes",
            security.max_admission_grant_bytes);
        security.max_token_decoded_bytes = optional_integer(
            ingress,
            "max_token_decoded_bytes",
            security.max_token_decoded_bytes);
        security.source_rate_per_second = optional_integer(
            ingress,
            "source_rate_per_second",
            security.source_rate_per_second);
        security.source_burst = optional_integer(
            ingress, "source_burst", security.source_burst);
        security.source_throttle_close_after = optional_integer(
            ingress,
            "source_throttle_close_after",
            security.source_throttle_close_after);
        security.session_attach_attempts = optional_integer(
            ingress,
            "session_attach_attempts",
            security.session_attach_attempts);
        security.concurrent_verifications = optional_integer(
            ingress,
            "concurrent_verifications",
            security.concurrent_verifications);
        security.max_tracked_sources = optional_integer(
            ingress,
            "max_tracked_sources",
            security.max_tracked_sources);
    }

    const sol::object admission_value = root.raw_get<sol::object>("admission");
    if (admission_value != sol::lua_nil) {
        if (!admission_value.is<sol::table>()) {
            throw std::invalid_argument("gateway config admission must be a table");
        }
        const auto admission = admission_value.as<sol::table>();
        config.login.identity_kid = optional_string(
            admission, "identity_kid", config.login.identity_kid);
        config.login.identity_issuer = optional_string(
            admission, "identity_issuer", config.login.identity_issuer);
        // 单 kid 的旧键名一律拒绝:静默忽略会让运维以为改生效了,而网关
        // 仍在用别的 kid 验签。
        if (admission.raw_get<sol::object>("admission_grant_kid") !=
            sol::lua_nil) {
            throw std::invalid_argument(
                "gateway config admission_grant_kid is retired; list every "
                "verification key under admission.grant_keys");
        }
        const sol::object grant_keys_value =
            admission.raw_get<sol::object>("grant_keys");
        if (grant_keys_value != sol::lua_nil) {
            if (!grant_keys_value.is<sol::table>()) {
                throw std::invalid_argument(
                    "gateway config admission.grant_keys must be a table");
            }
            const sol::table grant_keys = grant_keys_value.as<sol::table>();
            std::vector<AdmissionGrantKeySource> ring;
            ring.reserve(grant_keys.size());
            for (std::size_t index = 1; index <= grant_keys.size(); ++index) {
                const sol::object entry_value =
                    grant_keys.raw_get<sol::object>(index);
                if (!entry_value.is<sol::table>()) {
                    throw std::invalid_argument(
                        "gateway config admission.grant_keys entries must be "
                        "tables");
                }
                const sol::table entry = entry_value.as<sol::table>();
                AdmissionGrantKeySource source;
                source.kid = optional_string(entry, "kid", std::string{});
                source.public_key_environment = optional_string(
                    entry, "public_key_environment", std::string{});
                if (source.kid.empty() ||
                    source.public_key_environment.empty()) {
                    throw std::invalid_argument(
                        "gateway config admission.grant_keys entries need kid "
                        "and public_key_environment");
                }
                ring.push_back(std::move(source));
            }
            if (ring.empty()) {
                throw std::invalid_argument(
                    "gateway config admission.grant_keys must not be empty");
            }
            config.login.admission_grant_keys = std::move(ring);
        }
        config.login.admission_grant_issuer = optional_string(
            admission, "admission_grant_issuer", config.login.admission_grant_issuer);
        config.login.admission_grant_window = std::chrono::seconds{
            optional_integer<std::int64_t>(
                admission,
                "grant_window_seconds",
                config.login.admission_grant_window.count())};
        config.login.deployment_id = optional_string(
            admission, "deployment_id", config.login.deployment_id);
        config.login.admission_consumption_prefix = optional_string(
            admission, "consumption_prefix", config.login.admission_consumption_prefix);
        config.login.admission_reservation_ttl = std::chrono::seconds{
            optional_integer<std::int64_t>(
                admission,
                "reservation_ttl_seconds",
                config.login.admission_reservation_ttl.count())};
    }

    if (config.tick_rate == 0 || config.max_events_per_frame == 0) {
        throw std::invalid_argument(
            "gateway tick rate and max events per frame must be positive");
    }
    if (!config.downstream_address.empty() || config.downstream_port != 0) {
        config.login.static_realm = RealmEndpoint{
            config.downstream_address, config.downstream_port};
    }
    return config;
}

}  // namespace realm::game::gateway
