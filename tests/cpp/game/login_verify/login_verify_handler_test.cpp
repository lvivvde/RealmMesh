#include "realmmesh/game/login_verify/login_verify_handler.hpp"

#include "realmmesh/game/common/account_store.hpp"
#include "realmmesh/game/common/identity_token.hpp"
#include "realmmesh/game/common/json.hpp"
#include "realmmesh/test_support/temporary_directory.hpp"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace realm::game::login_verify {
namespace {

using common::JsonCodec;
using common::JsonObject;

// 只测 LoginVerifyHandler 外显行为:路由、分档拒绝与签发结果。
class LoginVerifyHandlerTest : public ::testing::Test {
protected:
    void SetUp() override {
        const auto path = directory_.path() / "accounts.lua";
        std::ofstream stream(path, std::ios::binary);
        stream << kAccounts;
        // 显式关闭:load 在同一函数里读文件,不关会读到缓冲区前的空文件。
        stream.close();
        store_ = std::make_unique<common::ConfigAccountStore>(
            common::ConfigAccountStore::load(path));
        codec_ = std::make_unique<common::IdentityTokenCodec>(
            common::parse_identity_seed_hex(kSeedHex), std::string(kKid));
        handler_ = std::make_unique<LoginVerifyHandler>(
            *store_,
            *codec_,
            [this] { return clock_(); },
            identity_token_issuer,
            identity_token_ttl);
    }

    [[nodiscard]] static std::chrono::system_clock::time_point clock_() {
        return std::chrono::system_clock::time_point{
            std::chrono::seconds{1'700'000'000}};
    }

    [[nodiscard]] network::Http1Response verify(
        std::string_view body,
        std::string_view target = "/v1/login/verify") const {
        return handler_->handle("POST", target, body);
    }

    static constexpr std::string_view kSeedHex =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    static constexpr std::string_view kKid = "login-verify-test-1";
    static constexpr std::string_view kAccounts = R"lua(
return {
    accounts = {
        { account = "player",   credential = "dev", whitelisted = true },
        { account = "pinned",   credential = "dev", whitelisted = true, account_id = 4242 },
        { account = "banned",   credential = "dev", whitelisted = true, banned = true },
        { account = "outsider", credential = "dev" },
    },
}
)lua";

    test_support::TemporaryDirectory directory_{"login-verify-handler-test-"};
    std::unique_ptr<common::ConfigAccountStore> store_;
    std::unique_ptr<common::IdentityTokenCodec> codec_;
    std::unique_ptr<LoginVerifyHandler> handler_;
};

// player 的 FNV-1a 派生 id(与 account_store_test 钉死的同一摘要值)。
constexpr std::uint64_t kPlayerDerivedId = 5008278420455340480ULL;

TEST_F(LoginVerifyHandlerTest, IssuesTokenForValidAccount) {
    const auto response = verify(R"({"account":"pinned","credential":"dev"})");
    ASSERT_EQ(response.status, 200);
    const auto payload = JsonCodec::decode(response.body);
    ASSERT_TRUE(payload.has_value());
    const auto* token =
        std::get_if<std::string>(&payload->at("identity_token"));
    ASSERT_NE(token, nullptr);
    EXPECT_EQ(std::get<std::string>(payload->at("account_id")), "4242");
    EXPECT_EQ(std::get<std::int64_t>(payload->at("expires_in")), 1800);

    const auto claims = codec_->validate(*token, identity_token_issuer, clock_());
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->account_id, 4242ULL);
    EXPECT_EQ(claims->issuer, identity_token_issuer);
    EXPECT_EQ(
        std::chrono::duration_cast<std::chrono::seconds>(
            claims->expires_at - claims->issued_at),
        identity_token_ttl);
    ASSERT_EQ(claims->jti.size(), 32U);
    for (const char character : claims->jti) {
        EXPECT_TRUE(
            (character >= '0' && character <= '9') ||
            (character >= 'a' && character <= 'f'));
    }
}

TEST_F(LoginVerifyHandlerTest, DerivedAccountIdIsReportedAsDecimalString) {
    const auto response = verify(R"({"account":"player","credential":"dev"})");
    ASSERT_EQ(response.status, 200);
    const auto payload = JsonCodec::decode(response.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(
        std::get<std::string>(payload->at("account_id")),
        std::to_string(kPlayerDerivedId));
}

TEST_F(LoginVerifyHandlerTest, UnknownAccountAndWrongCredentialIndistinguishable) {
    const auto unknown = verify(R"({"account":"ghost","credential":"dev"})");
    const auto wrong = verify(R"({"account":"player","credential":"nope"})");
    EXPECT_EQ(unknown.status, 401);
    EXPECT_EQ(wrong.status, 401);
    // 两种失败共用同一错误体,账号存在性不外泄。
    EXPECT_EQ(unknown.body, wrong.body);
    const auto payload = JsonCodec::decode(unknown.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(payload->at("code")), 1001);
}

TEST_F(LoginVerifyHandlerTest, BannedAccountReturns1002) {
    const auto response = verify(R"({"account":"banned","credential":"dev"})");
    EXPECT_EQ(response.status, 403);
    const auto payload = JsonCodec::decode(response.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(payload->at("code")), 1002);
}

TEST_F(LoginVerifyHandlerTest, UnwhitelistedAccountReturns1003) {
    const auto response = verify(R"({"account":"outsider","credential":"dev"})");
    EXPECT_EQ(response.status, 403);
    const auto payload = JsonCodec::decode(response.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(payload->at("code")), 1003);
}

TEST_F(LoginVerifyHandlerTest, MalformedRequestsReturn1000) {
    const auto not_json = verify("not-json");
    EXPECT_EQ(not_json.status, 400);
    const auto missing_credential =
        verify(R"({"account":"player"})");
    EXPECT_EQ(missing_credential.status, 400);
    const auto wrong_type = verify(R"({"account":1,"credential":"dev"})");
    EXPECT_EQ(wrong_type.status, 400);
    for (const auto* response : {&not_json, &missing_credential, &wrong_type}) {
        const auto payload = JsonCodec::decode(response->body);
        ASSERT_TRUE(payload.has_value());
        EXPECT_EQ(std::get<std::int64_t>(payload->at("code")), 1000);
    }
}

TEST_F(LoginVerifyHandlerTest, OversizedJsonBodyIsRejectedAsInvalidRequest) {
    // JsonCodec 上限 4KiB:超限请求不进凭据判定,归 400/1000(更大的
    // body 在更早一层由 HttpServer 协议级按 413 空体回绝,见 #39)。
    std::string oversized =
        R"({"account":"pinned","credential":"dev","pad":")";
    oversized += std::string(5000, 'a');
    oversized += R"("})";
    const auto response = verify(oversized);
    EXPECT_EQ(response.status, 400);
    const auto payload = JsonCodec::decode(response.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(payload->at("code")), 1000);
}

TEST_F(LoginVerifyHandlerTest, UnknownPathReturns404) {
    const auto response = handler_->handle("GET", "/nope", "");
    EXPECT_EQ(response.status, 404);
    const auto payload = JsonCodec::decode(response.body);
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(std::get<std::int64_t>(payload->at("code")), 1000);
}

TEST_F(LoginVerifyHandlerTest, WrongMethodReturns405) {
    EXPECT_EQ(handler_->handle("GET", "/v1/login/verify", "").status, 405);
    EXPECT_EQ(handler_->handle("POST", "/healthz", "").status, 405);
    EXPECT_EQ(handler_->handle("POST", "/.well-known/jwks.json", "").status, 405);
}

TEST_F(LoginVerifyHandlerTest, ServesJwksMatchingCodec) {
    const auto response = handler_->handle("GET", "/.well-known/jwks.json", "");
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, codec_->jwks());
    // JWKS 暴露的 kid 与自签 token 的 kid 同源(validate 内部按 kid 选键)。
    EXPECT_NE(response.body.find(std::string{kKid}), std::string::npos);
}

TEST_F(LoginVerifyHandlerTest, ServesHealthz) {
    const auto response = handler_->handle("GET", "/healthz", "");
    EXPECT_EQ(response.status, 200);
    EXPECT_EQ(response.body, "ok");
}

TEST_F(LoginVerifyHandlerTest, QueryStringIsIgnored) {
    const auto response = verify(
        R"({"account":"pinned","credential":"dev"})",
        "/v1/login/verify?src=test");
    EXPECT_EQ(response.status, 200);
}

TEST_F(LoginVerifyHandlerTest, JtiDiffersPerIssuance) {
    const auto first = verify(R"({"account":"pinned","credential":"dev"})");
    const auto second = verify(R"({"account":"pinned","credential":"dev"})");
    const auto first_payload = JsonCodec::decode(first.body);
    const auto second_payload = JsonCodec::decode(second.body);
    ASSERT_TRUE(first_payload.has_value());
    ASSERT_TRUE(second_payload.has_value());
    const auto first_claims = codec_->validate(
        std::get<std::string>(first_payload->at("identity_token")),
        identity_token_issuer,
        clock_());
    const auto second_claims = codec_->validate(
        std::get<std::string>(second_payload->at("identity_token")),
        identity_token_issuer,
        clock_());
    ASSERT_TRUE(first_claims.has_value());
    ASSERT_TRUE(second_claims.has_value());
    EXPECT_NE(first_claims->jti, second_claims->jti);
}

}  // namespace
}  // namespace realm::game::login_verify
