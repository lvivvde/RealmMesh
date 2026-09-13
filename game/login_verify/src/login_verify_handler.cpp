#include "realmmesh/game/login_verify/login_verify_handler.hpp"

#include "realmmesh/game/common/json.hpp"
#include "realmmesh/observability/metrics_registry.hpp"

#include <sodium.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace realm::game::login_verify {
namespace {

/// verify_reject_total 的 reason 标签(#34 定案 invalid|banned|
/// whitelist):1000 请求性错误并入 invalid(请求未构成有效验签)。
void count_reject(
    observability::MetricsRegistry* metrics, std::string_view reason) {
    if (metrics != nullptr) {
        metrics->counter_add(
            "verify_reject_total", 1.0, {{"reason", reason}});
    }
}

using common::JsonObject;
using common::JsonValue;

[[nodiscard]] network::Http1Response json_response(
    int status, const JsonObject& payload) {
    network::Http1Response response;
    response.status = status;
    response.headers.emplace_back("Content-Type", "application/json");
    response.body = common::JsonCodec::encode(payload);
    return response;
}

[[nodiscard]] network::Http1Response error_response(
    int status, int code, std::string_view message) {
    return json_response(
        status,
        {{"code", static_cast<std::int64_t>(code)},
         {"message", JsonValue(std::string(message))}});
}

[[nodiscard]] network::Http1Response plain_response(
    int status, std::string_view body) {
    network::Http1Response response;
    response.status = status;
    response.headers.emplace_back("Content-Type", "text/plain");
    response.body = std::string(body);
    return response;
}

/// origin-form 目标去掉 query 部分。
[[nodiscard]] std::string_view request_path(std::string_view target) {
    const std::size_t query = target.find('?');
    return query == std::string_view::npos ? target : target.substr(0, query);
}

/// jti:libsodium 随机 16 字节的 32 字符小写 hex(codec 的签发前置条件)。
[[nodiscard]] std::string random_jti() {
    if (sodium_init() < 0) {
        throw std::runtime_error("sodium_init failed");
    }
    std::array<unsigned char, 16> bytes{};
    randombytes_buf(bytes.data(), bytes.size());
    static constexpr char hex_digits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(bytes.size() * 2);
    for (const unsigned char byte : bytes) {
        hex.push_back(hex_digits[byte >> 4U]);
        hex.push_back(hex_digits[byte & 0x0FU]);
    }
    return hex;
}

}  // namespace

LoginVerifyHandler::LoginVerifyHandler(
    const common::AccountStore& store,
    const common::IdentityTokenCodec& codec,
    Clock clock,
    std::string_view issuer,
    std::chrono::seconds ttl,
    observability::MetricsRegistry* metrics)
    : store_(&store),
      codec_(&codec),
      clock_(std::move(clock)),
      issuer_(issuer),
      ttl_(ttl),
      metrics_(metrics) {}

network::Http1Response LoginVerifyHandler::handle(
    std::string_view method, std::string_view target,
    std::string_view body) const {
    const std::string_view path = request_path(target);

    if (path == "/v1/login/verify") {
        if (method != "POST") {
            return error_response(405, error_invalid_request, "method not allowed");
        }
        if (metrics_ != nullptr) {
            metrics_->counter_add("verify_requests_total");
        }
        const std::optional<JsonObject> request = common::JsonCodec::decode(body);
        if (!request.has_value()) {
            count_reject(metrics_, "invalid");
            return error_response(400, error_invalid_request, "malformed request");
        }
        const auto account = request->find("account");
        const auto credential = request->find("credential");
        if (account == request->end() || credential == request->end() ||
            !std::holds_alternative<std::string>(account->second) ||
            !std::holds_alternative<std::string>(credential->second)) {
            count_reject(metrics_, "invalid");
            return error_response(400, error_invalid_request, "malformed request");
        }
        const std::optional<common::AccountRecord> record =
            store_->authenticate(
                std::get<std::string>(account->second),
                std::get<std::string>(credential->second));
        if (!record.has_value()) {
            count_reject(metrics_, "invalid");
            return error_response(401, error_invalid_credentials, "invalid credentials");
        }
        if (record->banned) {
            count_reject(metrics_, "banned");
            return error_response(403, error_account_banned, "account banned");
        }
        if (!record->whitelisted) {
            count_reject(metrics_, "whitelist");
            return error_response(403, error_not_whitelisted, "account not whitelisted");
        }

        const auto now = clock_();
        const common::IdentityClaims claims{
            .issuer = issuer_,
            .account_id = record->account_id,
            .jti = random_jti(),
            .issued_at = now,
            .expires_at = now + ttl_,
        };
        // 签发时延(#34):Ed25519 签名的耗时,成功路径观测。
        const auto issue_started = std::chrono::steady_clock::now();
        const auto token = codec_->issue(claims);
        if (metrics_ != nullptr) {
            metrics_->histogram_observe(
                "verify_issue_duration_seconds",
                std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - issue_started)
                    .count());
        }
        return json_response(
            200,
            {{"identity_token", JsonValue(token)},
             {"account_id", JsonValue(std::to_string(record->account_id))},
             {"expires_in", static_cast<std::int64_t>(ttl_.count())}});
    }

    if (path == "/.well-known/jwks.json") {
        if (method != "GET") {
            return error_response(405, error_invalid_request, "method not allowed");
        }
        network::Http1Response response;
        response.status = 200;
        response.headers.emplace_back("Content-Type", "application/json");
        response.body = codec_->jwks();
        return response;
    }

    if (path == "/healthz") {
        if (method != "GET") {
            return error_response(405, error_invalid_request, "method not allowed");
        }
        return plain_response(200, "ok");
    }

    return error_response(404, error_invalid_request, "not found");
}

}  // namespace realm::game::login_verify
