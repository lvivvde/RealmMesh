#include "realmmesh/game/queue/queue_handler.hpp"

#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/json.hpp"
#include "realmmesh/game/queue/queue_core.hpp"
#include "realmmesh/game/queue/queue_number.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace realm::game::queue {
namespace {

using common::IdentityClaims;
using common::IdentityTokenCodec;
using common::JsonCodec;
using common::JsonObject;
using common::JsonValue;

/// 放行响应按 spec 携带嵌套 admit_grant(JsonCodec 只编扁平对象):
/// 截取嵌套对象解码;嵌套体内无子对象,首个 '}' 即闭合。
[[nodiscard]] std::optional<JsonObject> decode_admit_grant(
    std::string_view body) {
    static constexpr std::string_view marker = "\"admit_grant\":";
    const auto marker_at = body.find(marker);
    if (marker_at == std::string_view::npos) {
        return std::nullopt;
    }
    const auto start = marker_at + marker.size();
    const auto end = body.find('}', start);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    return JsonCodec::decode(body.substr(start, end - start + 1));
}

/// 只测 QueueHandler 外显行为:路由、Bearer 分档拒绝、幂等发号、
/// 位次与放行重签。时间注入可推进,验证号牌时效。
class QueueHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        identity_codec_ = std::make_unique<IdentityTokenCodec>(
            common::parse_identity_seed_hex(kSeedHex),
            std::string{kIdentityKid});
        number_codec_ = std::make_unique<QueueNumberCodec>(
            common::parse_identity_seed_hex(kSeedHex), std::string{kKid});
        core_ = std::make_unique<QueueCore>(100);
        handler_ = std::make_unique<QueueHandler>(
            *core_,
            *identity_codec_,
            *number_codec_,
            [this] { return now_; },
            std::string{kIssuer},
            std::chrono::seconds{3600},
            std::chrono::seconds{300});
    }

    /// jti 需 32 字符小写 hex;suffix 变体保证不同身份。
    [[nodiscard]] static std::string jti(std::string_view suffix) {
        std::string hex(32 - suffix.size(), 'a');
        hex += suffix;
        return hex;
    }

    [[nodiscard]] std::string identity_token(std::string_view jti_value) const {
        return identity_codec_->issue(IdentityClaims{
            .issuer = std::string{kIssuer},
            .account_id = 4242,
            .jti = std::string{jti_value},
            .issued_at = now_,
            .expires_at = now_ + std::chrono::seconds{1800},
        });
    }

    [[nodiscard]] static network::Http1Request request(
        std::string_view method,
        std::string_view target,
        const std::optional<std::string>& bearer = std::nullopt) {
        network::Http1Request request;
        request.method = std::string(method);
        request.target = std::string(target);
        if (bearer.has_value()) {
            request.headers.emplace_back("authorization", "Bearer " + *bearer);
        }
        return request;
    }

    [[nodiscard]] std::int64_t code_of(
        const network::Http1Response& response) const {
        const auto payload = JsonCodec::decode(response.body);
        EXPECT_TRUE(payload.has_value());
        if (!payload.has_value()) return 0;
        return std::get<std::int64_t>(payload->at("code"));
    }

    static constexpr std::string_view kSeedHex =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static constexpr std::string_view kKid = "queue-test-1";
    static constexpr std::string_view kIdentityKid = "login-verify-test-1";
    static constexpr std::string_view kIssuer = "realmmesh/login-verify";

    std::chrono::system_clock::time_point now_ =
        std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'000'000}};
    std::unique_ptr<IdentityTokenCodec> identity_codec_;
    std::unique_ptr<QueueNumberCodec> number_codec_;
    std::unique_ptr<QueueCore> core_;
    std::unique_ptr<QueueHandler> handler_;
};

TEST_F(QueueHandlerTest, IssuesSequentialNumbersForDistinctIdentities) {
    const auto first = handler_->handle(
        request("POST", "/v1/queue/tickets", identity_token(jti("01"))));
    ASSERT_EQ(first.status, 202);
    const auto first_payload = JsonCodec::decode(first.body);
    ASSERT_TRUE(first_payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(first_payload->at("number")), 1);

    const auto second = handler_->handle(
        request("POST", "/v1/queue/tickets", identity_token(jti("02"))));
    ASSERT_EQ(second.status, 202);
    const auto second_payload = JsonCodec::decode(second.body);
    ASSERT_TRUE(second_payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(second_payload->at("number")), 2);

    // 号牌可由号牌验签方解码:号值一致、未放行、时效为排队 TTL。
    const auto* token = std::get_if<std::string>(
        &first_payload->at("queue_number_token"));
    ASSERT_NE(token, nullptr);
    const auto claims = number_codec_->validate(*token, now_);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->number, 1U);
    EXPECT_FALSE(claims->admitted);
    EXPECT_EQ(
        std::chrono::duration_cast<std::chrono::seconds>(
            claims->expires_at - claims->issued_at),
        std::chrono::seconds{3600});
}

TEST_F(QueueHandlerTest, SameIdentityReplaysSameNumber) {
    const auto token = identity_token(jti("01"));
    const auto first = handler_->handle(
        request("POST", "/v1/queue/tickets", token));
    const auto second = handler_->handle(
        request("POST", "/v1/queue/tickets", token));
    ASSERT_EQ(first.status, 202);
    ASSERT_EQ(second.status, 202);
    const auto first_payload = JsonCodec::decode(first.body);
    const auto second_payload = JsonCodec::decode(second.body);
    ASSERT_TRUE(first_payload.has_value());
    ASSERT_TRUE(second_payload.has_value());
    EXPECT_EQ(
        std::get<std::int64_t>(first_payload->at("number")),
        std::get<std::int64_t>(second_payload->at("number")));
}

TEST_F(QueueHandlerTest, MissingOrMalformedBearerIs1001) {
    const auto absent = handler_->handle(request("POST", "/v1/queue/tickets"));
    EXPECT_EQ(absent.status, 401);
    EXPECT_EQ(code_of(absent), QueueHandler::error_invalid_credentials);

    const auto wrong_scheme = handler_->handle(request(
        "POST", "/v1/queue/tickets", std::string{}));
    EXPECT_EQ(wrong_scheme.status, 401);
    EXPECT_EQ(code_of(wrong_scheme), QueueHandler::error_invalid_credentials);

    network::Http1Request basic;
    basic.method = "POST";
    basic.target = "/v1/queue/tickets";
    basic.headers.emplace_back("authorization", "Basic dXNlcjpwYXNz");
    const auto basic_response = handler_->handle(basic);
    EXPECT_EQ(basic_response.status, 401);
    EXPECT_EQ(code_of(basic_response), QueueHandler::error_invalid_credentials);

    network::Http1Request empty_credential;
    empty_credential.method = "POST";
    empty_credential.target = "/v1/queue/tickets";
    empty_credential.headers.emplace_back("authorization", "Bearer ");
    const auto empty_response = handler_->handle(empty_credential);
    EXPECT_EQ(empty_response.status, 401);
    EXPECT_EQ(code_of(empty_response), QueueHandler::error_invalid_credentials);
}

TEST_F(QueueHandlerTest, InvalidIdentityIs1001) {
    // 被篡改的身份 Token:换掉签名段末字符(仍三段式,必验签失败)。
    std::string token = identity_token(jti("01"));
    ASSERT_FALSE(token.empty());
    token.back() = token.back() == 'a' ? 'b' : 'a';
    const auto tampered = handler_->handle(
        request("POST", "/v1/queue/tickets", token));
    EXPECT_EQ(tampered.status, 401);
    EXPECT_EQ(code_of(tampered), QueueHandler::error_invalid_credentials);

    // 非本服务签发(issuer 不符)。
    const auto foreign = handler_->handle(
        request(
            "POST",
            "/v1/queue/tickets",
            identity_codec_->issue(IdentityClaims{
                .issuer = "other/realm",
                .account_id = 4242,
                .jti = jti("03"),
                .issued_at = now_,
                .expires_at = now_ + std::chrono::seconds{1800},
            })));
    EXPECT_EQ(foreign.status, 401);
    EXPECT_EQ(code_of(foreign), QueueHandler::error_invalid_credentials);

    // 过期(now 越过 exp + 宽限)。
    const auto expired_token = identity_codec_->issue(IdentityClaims{
        .issuer = std::string{kIssuer},
        .account_id = 4242,
        .jti = jti("04"),
        .issued_at = now_,
        .expires_at = now_,
    });
    now_ += std::chrono::seconds{1800};
    const auto expired = handler_->handle(
        request("POST", "/v1/queue/tickets", expired_token));
    EXPECT_EQ(expired.status, 401);
    EXPECT_EQ(code_of(expired), QueueHandler::error_invalid_credentials);
}

TEST_F(QueueHandlerTest, ProgressReportsWaterLevelsAndCachePolicy) {
    const auto response = handler_->handle(
        request("GET", "/v1/queue/progress"));
    ASSERT_EQ(response.status, 200);
    bool has_cache_control = false;
    for (const auto& [name, value] : response.headers) {
        if (name == "Cache-Control" && value == "public, max-age=1") {
            has_cache_control = true;
        }
    }
    EXPECT_TRUE(has_cache_control);

    const auto payload = JsonCodec::decode(response.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(payload->at("released_number")), 0);
    EXPECT_EQ(std::get<std::int64_t>(payload->at("admit_rate")), 0);
    EXPECT_EQ(
        std::get<std::int64_t>(payload->at("server_time")),
        1'700'000'000);

    // 放行后水位与实测速率进响应:发 30 号、步长内一次放 30 → 30/10s。
    for (int index = 0; index < 30; ++index) {
        ASSERT_EQ(
            handler_
                ->handle(request(
                    "POST",
                    "/v1/queue/tickets",
                    identity_token(jti(std::to_string(index)))))
                .status,
            202);
    }
    const auto batch = core_->release_batch(
        BudgetAggregate{.gateway_admission = 100, .realm_connections = 100},
        now_);
    ASSERT_EQ(batch, 30U);
    const auto after = handler_->handle(request("GET", "/v1/queue/progress"));
    const auto after_payload = JsonCodec::decode(after.body);
    ASSERT_TRUE(after_payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(after_payload->at("released_number")), 30);
    EXPECT_EQ(std::get<std::int64_t>(after_payload->at("admit_rate")), 3);
}

TEST_F(QueueHandlerTest, QueuedNumberReportsLivePosition) {
    const auto first = handler_->handle(
        request("POST", "/v1/queue/tickets", identity_token(jti("01"))));
    const auto second = handler_->handle(
        request("POST", "/v1/queue/tickets", identity_token(jti("02"))));
    ASSERT_EQ(first.status, 202);
    ASSERT_EQ(second.status, 202);

    // 放 1 号:1 号转放行,2 号仍在队,位次 = 2 - 1 = 1。
    ASSERT_EQ(
        core_->release_batch(
            BudgetAggregate{.gateway_admission = 100, .realm_connections = 1},
            now_),
        1U);

    const auto second_payload = JsonCodec::decode(second.body);
    ASSERT_TRUE(second_payload.has_value());
    const auto* queued_token =
        std::get_if<std::string>(&second_payload->at("queue_number_token"));
    ASSERT_NE(queued_token, nullptr);
    const auto queued = handler_->handle(
        request("GET", "/v1/queue/tickets/me", *queued_token));
    ASSERT_EQ(queued.status, 200);
    const auto payload = JsonCodec::decode(queued.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::string>(payload->at("status")), "queued");
    EXPECT_EQ(std::get<std::int64_t>(payload->at("position")), 1);
    // 速率为 0(窗口未建立)时估时取 0(spec 裁决)。
    EXPECT_EQ(std::get<std::int64_t>(payload->at("estimated_wait_seconds")), 0);
    // 未放行响应不带放行凭证。
    EXPECT_EQ(payload->find("queue_number_token"), payload->end());
}

TEST_F(QueueHandlerTest, AdmittedNumberGetsResignedGrant) {
    const auto issued = handler_->handle(
        request("POST", "/v1/queue/tickets", identity_token(jti("01"))));
    ASSERT_EQ(issued.status, 202);
    const auto issued_payload = JsonCodec::decode(issued.body);
    ASSERT_TRUE(issued_payload.has_value());
    const auto* token =
        std::get_if<std::string>(&issued_payload->at("queue_number_token"));
    ASSERT_NE(token, nullptr);

    ASSERT_EQ(
        core_->release_batch(
            BudgetAggregate{.gateway_admission = 100, .realm_connections = 100},
            now_),
        1U);

    const auto admitted = handler_->handle(
        request("GET", "/v1/queue/tickets/me", *token));
    ASSERT_EQ(admitted.status, 200);
    // 外层契约(§5.1):状态/位次/估时,无散置凭证字段;凭证嵌套在
    // admit_grant 内且含号值(encode 键序确定,首尾段可比对)。
    EXPECT_TRUE(std::string_view{admitted.body}.starts_with(
        R"({"admit_grant":{"expires_in":300,"number":1,"queue_number_token":")"));
    EXPECT_TRUE(std::string_view{admitted.body}.ends_with(
        R"(,"estimated_wait_seconds":0,"position":0,"status":"admitted"})"));
    const auto grant_payload = decode_admit_grant(admitted.body);
    ASSERT_TRUE(grant_payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(grant_payload->at("number")), 1);
    EXPECT_EQ(std::get<std::int64_t>(grant_payload->at("expires_in")), 300);

    // 放行凭证 = 重签号牌:admitted 置位、号值不变、时效为放行宽限。
    const auto* grant = std::get_if<std::string>(
        &grant_payload->at("queue_number_token"));
    ASSERT_NE(grant, nullptr);
    const auto grant_claims = number_codec_->validate(*grant, now_);
    ASSERT_TRUE(grant_claims.has_value());
    EXPECT_EQ(grant_claims->number, 1U);
    EXPECT_TRUE(grant_claims->admitted);
    EXPECT_EQ(
        std::chrono::duration_cast<std::chrono::seconds>(
            grant_claims->expires_at - grant_claims->issued_at),
        std::chrono::seconds{300});

    // 凭证再查仍为放行(宽限自重签起算,重新拿新凭证)。
    const auto again = handler_->handle(
        request("GET", "/v1/queue/tickets/me", *grant));
    ASSERT_EQ(again.status, 200);
    const auto again_payload = decode_admit_grant(again.body);
    ASSERT_TRUE(again_payload.has_value());
    EXPECT_TRUE(std::string_view{again.body}.ends_with(
        R"("status":"admitted"})"));
}

TEST_F(QueueHandlerTest, InvalidOrExpiredNumberIs2001) {
    const auto absent = handler_->handle(request("GET", "/v1/queue/tickets/me"));
    EXPECT_EQ(absent.status, 401);
    EXPECT_EQ(code_of(absent), QueueHandler::error_invalid_number);

    const auto garbage = handler_->handle(
        request("GET", "/v1/queue/tickets/me", std::string{"not-a-token"}));
    EXPECT_EQ(garbage.status, 401);
    EXPECT_EQ(code_of(garbage), QueueHandler::error_invalid_number);

    // 过期号牌:签发于 now,查询时已越过 exp + 宽限。
    const auto stale = number_codec_->issue(QueueNumberClaims{
        .number = 1,
        .admitted = false,
        .issued_at = now_,
        .expires_at = now_,
    });
    now_ += std::chrono::seconds{3600};
    const auto expired = handler_->handle(
        request("GET", "/v1/queue/tickets/me", stale));
    EXPECT_EQ(expired.status, 401);
    EXPECT_EQ(code_of(expired), QueueHandler::error_invalid_number);
}

TEST_F(QueueHandlerTest, WrongMethodAndUnknownPath) {
    EXPECT_EQ(
        handler_->handle(request("GET", "/v1/queue/tickets")).status, 405);
    EXPECT_EQ(
        handler_->handle(request("POST", "/v1/queue/progress")).status, 405);
    EXPECT_EQ(
        handler_->handle(request("POST", "/v1/queue/tickets/me")).status, 405);
    EXPECT_EQ(handler_->handle(request("POST", "/healthz")).status, 405);
    EXPECT_EQ(handler_->handle(request("GET", "/nope")).status, 404);
}

TEST_F(QueueHandlerTest, HealthzAndQueryStrings) {
    const auto health = handler_->handle(request("GET", "/healthz"));
    EXPECT_EQ(health.status, 200);
    EXPECT_EQ(health.body, "ok");

    const auto response = handler_->handle(
        request("POST", "/v1/queue/tickets?src=test", identity_token(jti("01"))));
    EXPECT_EQ(response.status, 202);
    const auto progress =
        handler_->handle(request("GET", "/v1/queue/progress?brief=1"));
    EXPECT_EQ(progress.status, 200);
}

}  // namespace
}  // namespace realm::game::queue
