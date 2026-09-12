#include "realmmesh/game/queue/queue_handler.hpp"

#include "realmmesh/game/common/json.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace realm::game::queue {
namespace {

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

/// origin-form 目标去掉 query 部分(先例同 login_verify)。
[[nodiscard]] std::string_view request_path(std::string_view target) {
    const std::size_t query = target.find('?');
    return query == std::string_view::npos ? target : target.substr(0, query);
}

/// RFC 6750 Bearer 凭据:Authorization 头缺失、方案不符或凭据为空
/// 均视为无凭据。
[[nodiscard]] std::optional<std::string_view> bearer_token(
    const network::Http1Request& request) {
    const std::string* authorization = request.header("Authorization");
    if (authorization == nullptr) return std::nullopt;
    static constexpr std::string_view scheme = "Bearer ";
    if (authorization->size() <= scheme.size() ||
        !std::equal(
            scheme.begin(), scheme.end(), authorization->begin(),
            [](char expected, char actual) {
                return expected == actual;
            })) {
        return std::nullopt;
    }
    return std::string_view{*authorization}.substr(scheme.size());
}

/// 估时(§5.1/ADR-0006):ceil(位次/速率);速率为 0(未建立)或位次
/// 为 0 时取 0,客户端以本地插值为准(spec #42 裁决)。
[[nodiscard]] std::int64_t estimate_wait(
    std::uint64_t position, std::uint64_t rate) {
    if (position == 0 || rate == 0) {
        return 0;
    }
    return static_cast<std::int64_t>((position + rate - 1U) / rate);
}

}  // namespace

QueueHandler::QueueHandler(
    QueueCore& core,
    const common::IdentityTokenCodec& identity_codec,
    const QueueTicketCodec& ticket_codec,
    Clock clock,
    std::string_view identity_issuer,
    std::chrono::seconds queued_ticket_ttl,
    std::chrono::seconds admit_grace)
    : core_(&core),
      identity_codec_(&identity_codec),
      ticket_codec_(&ticket_codec),
      clock_(std::move(clock)),
      identity_issuer_(identity_issuer),
      queued_ticket_ttl_(queued_ticket_ttl),
      admit_grace_(admit_grace) {}

network::Http1Response QueueHandler::handle(
    const network::Http1Request& request) const {
    const std::string_view path = request_path(request.target);

    if (path == "/v1/queue/tickets") {
        if (request.method != "POST") {
            return error_response(
                405, error_invalid_request, "method not allowed");
        }
        const auto now = clock_();
        const auto credential = bearer_token(request);
        if (!credential.has_value()) {
            return error_response(
                401, error_invalid_credentials, "missing bearer credential");
        }
        const auto identity = identity_codec_->validate(
            *credential, identity_issuer_, now);
        if (!identity.has_value()) {
            return error_response(
                401, error_invalid_credentials, "invalid credentials");
        }
        const auto issued = core_->issue(identity->jti, now);
        const auto token = ticket_codec_->issue(QueueTicketClaims{
            .number = issued.number,
            .admitted = false,
            .issued_at = now,
            .expires_at = now + queued_ticket_ttl_,
        });
        return json_response(
            202,
            {{"queue_number_token", JsonValue(std::move(token))},
             {"number", static_cast<std::int64_t>(issued.number)},
             {"estimated_wait_seconds",
              estimate_wait(
                  issued.number - core_->released_number(),
                  core_->admit_rate(now))}});
    }

    if (path == "/v1/queue/progress") {
        if (request.method != "GET") {
            return error_response(
                405, error_invalid_request, "method not allowed");
        }
        const auto now = clock_();
        network::Http1Response response = json_response(
            200,
            {{"released_number",
              static_cast<std::int64_t>(core_->released_number())},
             {"admit_rate", static_cast<std::int64_t>(core_->admit_rate(now))},
             {"server_time",
              std::chrono::duration_cast<std::chrono::seconds>(
                  now.time_since_epoch())
                  .count()}});
        // CDN 卸载契约(ADR-0006):全局单调量,缓存 1s。
        response.headers.emplace_back("Cache-Control", "public, max-age=1");
        return response;
    }

    if (path == "/v1/queue/tickets/me") {
        if (request.method != "GET") {
            return error_response(
                405, error_invalid_request, "method not allowed");
        }
        const auto now = clock_();
        const auto credential = bearer_token(request);
        if (!credential.has_value()) {
            return error_response(
                401, error_invalid_ticket, "missing bearer ticket");
        }
        const auto claims = ticket_codec_->validate(*credential, now);
        if (!claims.has_value()) {
            return error_response(
                401, error_invalid_ticket, "invalid or expired queue ticket");
        }
        if (claims->admitted || claims->number <= core_->released_number()) {
            // 放行凭证 = 号牌重签 admitted(ADR-0006);宽限自重签起算。
            const auto grant = ticket_codec_->issue(QueueTicketClaims{
                .number = claims->number,
                .admitted = true,
                .issued_at = now,
                .expires_at = now + admit_grace_,
            });
            return json_response(
                200,
                {{"status", std::string{"admitted"}},
                 {"position", std::int64_t{0}},
                 {"estimated_wait_seconds", std::int64_t{0}},
                 {"queue_number_token", JsonValue(std::move(grant))},
                 {"expires_in",
                  static_cast<std::int64_t>(admit_grace_.count())}});
        }
        const auto position = claims->number - core_->released_number();
        return json_response(
            200,
            {{"status", std::string{"queued"}},
             {"position", static_cast<std::int64_t>(position)},
             {"estimated_wait_seconds",
              estimate_wait(position, core_->admit_rate(now))}});
    }

    if (path == "/healthz") {
        if (request.method != "GET") {
            return error_response(
                405, error_invalid_request, "method not allowed");
        }
        return plain_response(200, "ok");
    }

    return error_response(404, error_invalid_request, "not found");
}

}  // namespace realm::game::queue
