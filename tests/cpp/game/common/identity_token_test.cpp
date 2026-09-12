#include "realmmesh/game/common/identity_token.hpp"

#include <gtest/gtest.h>

#include <sodium.h>

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "realmmesh/game/common/base64url.hpp"
#include "realmmesh/game/common/json.hpp"

namespace realm::game::common {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view rfc8037_a3_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

std::chrono::system_clock::time_point at_time(std::int64_t seconds) {
    return std::chrono::system_clock::time_point(std::chrono::seconds{seconds});
}

// 测试自带的 hex 解码,独立于被测的 parse_identity_seed_hex。
Ed25519Seed seed_from(std::string_view hex) {
    Ed25519Seed seed{};
    for (std::size_t index = 0; index < seed.size(); ++index) {
        const auto nibble = [&](char value) {
            return value <= '9' ? value - '0' : (value | 0x20) - 'a' + 10;
        };
        seed[index] =
            static_cast<std::byte>((nibble(hex[index * 2]) << 4) |
                                   nibble(hex[index * 2 + 1]));
    }
    return seed;
}

Ed25519Seed test_seed() {
    return seed_from(rfc8037_a3_seed_hex);
}

IdentityTokenCodec test_codec() {
    return IdentityTokenCodec(test_seed(), "test-kid");
}

IdentityClaims test_claims() {
    return {
        .issuer = "https://login.realmmesh.example",
        .account_id = 12345678901234567890ULL,
        .jti = "0f1e2d3c4b5a69788796a5b4c3d2e1f0",
        .issued_at = at_time(1'700'000'000),
        .expires_at = at_time(1'700'001'800),
    };
}

JsonObject valid_header(std::string kid = "test-kid") {
    return {
        {"alg", std::string{"EdDSA"}},
        {"typ", std::string{"JWT"}},
        {"kid", std::move(kid)},
    };
}

JsonObject valid_payload() {
    return {
        {"iss", std::string{"https://login.realmmesh.example"}},
        {"sub", std::string{"12345678901234567890"}},
        {"aud", std::string{"realmmesh-access"}},
        {"purpose", std::string{"access"}},
        {"iat", static_cast<std::int64_t>(1'700'000'000)},
        {"exp", static_cast<std::int64_t>(1'700'001'800)},
        {"jti", std::string{"0f1e2d3c4b5a69788796a5b4c3d2e1f0"}},
    };
}

// 测试持有种子,可以为任意(含恶意)的 header/payload 造出合法签名,
// 以覆盖 issue() 永远不会产出、但 validate() 必须拒绝的分支。
std::string craft_token(
    const Ed25519Seed& seed,
    const JsonObject& header,
    const JsonObject& payload,
    bool sign = true) {
    if (sodium_init() < 0) {
        throw std::runtime_error("failed to initialize libsodium");
    }
    unsigned char public_key[crypto_sign_PUBLICKEYBYTES];
    unsigned char secret_key[crypto_sign_SECRETKEYBYTES];
    crypto_sign_seed_keypair(
        public_key,
        secret_key,
        reinterpret_cast<const unsigned char*>(seed.data()));
    const auto header_json = JsonCodec::encode(header);
    const auto payload_json = JsonCodec::encode(payload);
    const auto signing_input =
        base64url_encode(std::as_bytes(std::span{header_json})) + "." +
        base64url_encode(std::as_bytes(std::span{payload_json}));
    if (!sign) {
        return signing_input + ".";
    }
    unsigned char signature[crypto_sign_BYTES];
    unsigned long long signature_length = 0;
    crypto_sign_detached(
        signature,
        &signature_length,
        reinterpret_cast<const unsigned char*>(signing_input.data()),
        signing_input.size(),
        secret_key);
    return signing_input + "." +
        base64url_encode(std::as_bytes(std::span{signature}));
}

// 把指定位置换成另一个仍在 base64url 字母表内的字符:语法仍合法,
// 但签名/语义必然失配。
std::string flip_char(std::string_view token, std::size_t index) {
    std::string flipped{token};
    flipped[index] = flipped[index] == 'A' ? 'B' : 'A';
    return flipped;
}

// 互验 fixture(两条,同一种子、同 claims,签名实现不同):
// RFC 8037 A.3 种子 9d61b19d…7f60 派生 Ed25519 私钥,公钥 = A.3 向量
// d75a9801…511a;签名均为 64B 裸 R‖S。
// ① Node(crypto.sign,PKCS8 DER = 302e020100300506032b657004220420 ‖ 种子):
//    payload 键序与 JsonCodec 输出一致(排序)。
// ② PyJWT(jwt.encode(payload, Ed25519PrivateKey, algorithm="EdDSA")):
//    payload 保持字典插入序(iss 在前),恰好证明解析与键序无关。
constexpr std::string_view node_signed_token =
    "eyJhbGciOiJFZERTQSIsImtpZCI6InRlc3Qta2lkIiwidHlwIjoiSldUIn0"
    ".eyJhdWQiOiJyZWFsbW1lc2gtYWNjZXNzIiwiZXhwIjoxNzAwMDAxODAwLCJpYXQiOjE3MD"
    "AwMDAwMDAsImlzcyI6Imh0dHBzOi8vbG9naW4ucmVhbG1tZXNoLmV4YW1wbGUiLCJqdGkiO"
    "iIwZjFlMmQzYzRiNWE2OTc4ODc5NmE1YjRjM2QyZTFmMCIsInB1cnBvc2UiOiJhY2Nlc3Mi"
    "LCJzdWIiOiIxMjM0NTY3ODkwMTIzNDU2Nzg5MCJ9"
    ".oOqYgON_7BzQxDrLxkHL0Yr1E65CtlaQ1IxdW_Ksd22qg8uZksYx1sp07u2uaxLrbmBD0f"
    "08e3rKswkLTXEzAA";

constexpr std::string_view pyjwt_signed_token =
    "eyJhbGciOiJFZERTQSIsImtpZCI6InRlc3Qta2lkIiwidHlwIjoiSldUIn0"
    ".eyJpc3MiOiJodHRwczovL2xvZ2luLnJlYWxtbWVzaC5leGFtcGxlIiwic3ViIjoiMTIzND"
    "U2Nzg5MDEyMzQ1Njc4OTAiLCJhdWQiOiJyZWFsbW1lc2gtYWNjZXNzIiwicHVycG9zZSI6I"
    "mFjY2VzcyIsImlhdCI6MTcwMDAwMDAwMCwiZXhwIjoxNzAwMDAxODAwLCJqdGkiOiIwZjFl"
    "MmQzYzRiNWE2OTc4ODc5NmE1YjRjM2QyZTFmMCJ9"
    ".rthuX3gRg-bePUtDyytmqhU3OfNsgsK_Rf0H-zFQ9xQnY2eSoGlpFRfRF-gTzu7YcR9MLv"
    "cUMSVp-xpGlwMBAA";

TEST(IdentityTokenTest, IssuesDeterministicCompactJwsWithControlledHeader) {
    const auto token = test_codec().issue(test_claims());
    EXPECT_EQ(token, test_codec().issue(test_claims()));
    ASSERT_EQ(std::ranges::count(token, '.'), 2);
    EXPECT_EQ(token.find('='), std::string::npos);
    EXPECT_EQ(token.find('+'), std::string::npos);
    EXPECT_EQ(token.find('/'), std::string::npos);

    const auto first = token.find('.');
    const auto header_segment = token.substr(0, first);
    const auto header_bytes = base64url_decode(header_segment);
    ASSERT_TRUE(header_bytes.has_value());
    const auto header = JsonCodec::decode(std::string_view{
        reinterpret_cast<const char*>(header_bytes->data()),
        header_bytes->size()});
    ASSERT_TRUE(header.has_value());
    EXPECT_EQ(
        *header,
        JsonObject({{"alg", std::string{"EdDSA"}},
                    {"typ", std::string{"JWT"}},
                    {"kid", std::string{"test-kid"}}}));
}

TEST(IdentityTokenTest, RoundTripsClaimsThroughValidate) {
    IdentityTokenCodec codec(test_seed(), "test-kid");
    const auto token = codec.issue(test_claims());
    const auto claims = codec.validate(
        token, test_claims().issuer, at_time(1'700'000'000) + 1s);
    ASSERT_TRUE(claims.has_value());
    EXPECT_EQ(claims->issuer, "https://login.realmmesh.example");
    EXPECT_EQ(claims->account_id, 12345678901234567890ULL);
    EXPECT_EQ(claims->jti, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
    EXPECT_EQ(claims->issued_at, at_time(1'700'000'000));
    EXPECT_EQ(claims->expires_at, at_time(1'700'001'800));
}

TEST(IdentityTokenTest, AcceptsInteropFixturesFromNodeAndPyjwt) {
    for (const auto token : {node_signed_token, pyjwt_signed_token}) {
        const auto claims = test_codec().validate(
            token,
            "https://login.realmmesh.example",
            at_time(1'700'000'000));
        ASSERT_TRUE(claims.has_value());
        EXPECT_EQ(claims->account_id, 12345678901234567890ULL);
        EXPECT_EQ(claims->jti, "0f1e2d3c4b5a69788796a5b4c3d2e1f0");
        EXPECT_EQ(claims->issued_at, at_time(1'700'000'000));
        EXPECT_EQ(claims->expires_at, at_time(1'700'001'800));
    }
}

TEST(IdentityTokenTest, RejectsTamperedInteropFixtures) {
    const auto issuer = test_claims().issuer;
    const auto now = at_time(1'700'000'000);
    for (const auto token : {node_signed_token, pyjwt_signed_token}) {
        const auto first = token.find('.');
        const auto second = token.find('.', first + 1);
        EXPECT_FALSE(test_codec().validate(flip_char(token, 0), issuer, now)
                         .has_value());
        EXPECT_FALSE(
            test_codec()
                .validate(flip_char(token, first + 1), issuer, now)
                .has_value());
        EXPECT_FALSE(
            test_codec()
                .validate(flip_char(token, second + 1), issuer, now)
                .has_value());
    }
}

TEST(IdentityTokenTest, RejectsAnyTamperedSegment) {
    IdentityTokenCodec codec(test_seed(), "test-kid");
    const auto token = codec.issue(test_claims());
    const auto first = token.find('.');
    const auto second = token.find('.', first + 1);
    EXPECT_FALSE(
        codec.validate(flip_char(token, 0), test_claims().issuer, at_time(1'700'000'000))
            .has_value());
    EXPECT_FALSE(
        codec.validate(
            flip_char(token, first + 1), test_claims().issuer,
            at_time(1'700'000'000))
            .has_value());
    EXPECT_FALSE(
        codec.validate(
            flip_char(token, second + 1), test_claims().issuer,
            at_time(1'700'000'000))
            .has_value());
}

TEST(IdentityTokenTest, RejectsStructuralGarbage) {
    const auto expected_issuer = test_claims().issuer;
    const auto now = at_time(1'700'000'000);
    EXPECT_FALSE(test_codec().validate("", expected_issuer, now).has_value());
    EXPECT_FALSE(
        test_codec().validate("a.b", expected_issuer, now).has_value());
    EXPECT_FALSE(
        test_codec().validate("a.b.c.d", expected_issuer, now).has_value());
    EXPECT_FALSE(
        test_codec().validate("..", expected_issuer, now).has_value());
    EXPECT_FALSE(
        test_codec().validate("!!.!.?", expected_issuer, now).has_value());
}

TEST(IdentityTokenTest, RejectsWrongExpectedIssuer) {
    const auto token = test_codec().issue(test_claims());
    EXPECT_FALSE(
        test_codec()
            .validate(
                token, "https://evil.example", at_time(1'700'000'000))
            .has_value());
}

TEST(IdentityTokenTest, RejectsForeignAlgorithmTypAndUnsignedTokens) {
    const auto now = at_time(1'700'000'000);
    const auto expected_issuer = test_claims().issuer;

    auto none_header = valid_header();
    none_header["alg"] = std::string{"none"};
    EXPECT_FALSE(
        test_codec()
            .validate(
                craft_token(test_seed(), none_header, valid_payload(), false),
                expected_issuer,
                now)
            .has_value());

    auto hs_header = valid_header();
    hs_header["alg"] = std::string{"HS256"};
    EXPECT_FALSE(
        test_codec()
            .validate(
                craft_token(test_seed(), hs_header, valid_payload()),
                expected_issuer,
                now)
            .has_value());

    auto jws_header = valid_header();
    jws_header["typ"] = std::string{"JWS"};
    EXPECT_FALSE(
        test_codec()
            .validate(
                craft_token(test_seed(), jws_header, valid_payload()),
                expected_issuer,
                now)
            .has_value());
}

TEST(IdentityTokenTest, RejectsPinnedAudienceAndPurpose) {
    const auto now = at_time(1'700'000'000);
    const auto expected_issuer = test_claims().issuer;

    auto evil_audience = valid_payload();
    evil_audience["aud"] = std::string{"other-service"};
    EXPECT_FALSE(
        test_codec()
            .validate(
                craft_token(test_seed(), valid_header(), evil_audience),
                expected_issuer,
                now)
            .has_value());

    auto wrong_purpose = valid_payload();
    wrong_purpose["purpose"] = std::string{"login"};
    EXPECT_FALSE(
        test_codec()
            .validate(
                craft_token(test_seed(), valid_header(), wrong_purpose),
                expected_issuer,
                now)
            .has_value());
}

TEST(IdentityTokenTest, RejectsForeignKid) {
    const auto now = at_time(1'700'000'000);
    const auto expected_issuer = test_claims().issuer;

    EXPECT_FALSE(
        test_codec()
            .validate(
                craft_token(
                    test_seed(), valid_header("other-kid"), valid_payload()),
                expected_issuer,
                now)
            .has_value());

    IdentityTokenCodec other_codec(test_seed(), "kid-a");
    const auto token = other_codec.issue(test_claims());
    IdentityTokenCodec renamed_codec(test_seed(), "kid-b");
    EXPECT_FALSE(
        renamed_codec.validate(token, expected_issuer, now).has_value());
}

TEST(IdentityTokenTest, RejectsMalformedSub) {
    const auto now = at_time(1'700'000'000);
    const auto expected_issuer = test_claims().issuer;
    for (const auto* sub :
         {"abc", "", "007", "1.5", "-1", "+1", " 1", "0",
          "18446744073709551616"}) {
        auto payload = valid_payload();
        payload["sub"] = std::string{sub};
        EXPECT_FALSE(
            test_codec()
                .validate(
                    craft_token(test_seed(), valid_header(), payload),
                    expected_issuer,
                    now)
                .has_value())
            << "sub = " << sub;
    }

    auto maximal = valid_payload();
    maximal["sub"] = std::string{"18446744073709551615"};
    const auto accepted =
        test_codec()
            .validate(
                craft_token(test_seed(), valid_header(), maximal),
                expected_issuer,
                now)
            .has_value();
    EXPECT_TRUE(accepted);
}

TEST(IdentityTokenTest, EnforcesLowercaseHexJti) {
    const auto now = at_time(1'700'000'000);
    const auto expected_issuer = test_claims().issuer;
    for (const auto* jti :
         {"0f1e2d3c4b5a69788796a5b4c3d2e1f", "0f1e2d3c4b5a69788796a5b4c3d2e1f00",
          "0F1E2D3C4B5A69788796A5B4C3D2E1F0",
          "0f1e2d3c4b5a69788796a5b4c3d2e1fg"}) {
        auto payload = valid_payload();
        payload["jti"] = std::string{jti};
        EXPECT_FALSE(
            test_codec()
                .validate(
                    craft_token(test_seed(), valid_header(), payload),
                    expected_issuer,
                    now)
                .has_value())
            << "jti = " << jti;
    }
}

TEST(IdentityTokenTest, RejectsExactKeySetViolations) {
    const auto now = at_time(1'700'000'000);
    const auto expected_issuer = test_claims().issuer;
    const auto rejects = [&](const JsonObject& header,
                             const JsonObject& payload) {
        return !test_codec()
                    .validate(
                        craft_token(test_seed(), header, payload),
                        expected_issuer,
                        now)
                    .has_value();
    };

    auto missing_jti = valid_payload();
    missing_jti.erase("jti");
    EXPECT_TRUE(rejects(valid_header(), missing_jti));

    auto extra_member = valid_payload();
    extra_member["scope"] = std::string{"read"};
    EXPECT_TRUE(rejects(valid_header(), extra_member));

    auto string_iat = valid_payload();
    string_iat["iat"] = std::string{"1700000000"};
    EXPECT_TRUE(rejects(valid_header(), string_iat));

    auto bool_exp = valid_payload();
    bool_exp["exp"] = true;
    EXPECT_TRUE(rejects(valid_header(), bool_exp));

    auto missing_typ = valid_header();
    missing_typ.erase("typ");
    EXPECT_TRUE(rejects(missing_typ, valid_payload()));

    auto extra_header_member = valid_header();
    extra_header_member["cty"] = std::string{"JWT"};
    EXPECT_TRUE(rejects(extra_header_member, valid_payload()));

    auto int_kid = valid_header();
    int_kid["kid"] = static_cast<std::int64_t>(7);
    EXPECT_TRUE(rejects(int_kid, valid_payload()));
}

TEST(IdentityTokenTest, RejectsTokenSignedByDifferentKey) {
    Ed25519Seed other_seed{};
    other_seed.front() = std::byte{2};
    EXPECT_FALSE(
        test_codec()
            .validate(
                craft_token(other_seed, valid_header(), valid_payload()),
                test_claims().issuer,
                at_time(1'700'000'000))
            .has_value());
}

TEST(IdentityTokenTest, EnforcesExpiryLeewayBoundary) {
    const auto now = at_time(1'700'000'000);
    IdentityTokenCodec codec(test_seed(), "test-kid");
    auto claims = test_claims();
    claims.expires_at = now;
    const auto token = codec.issue(claims);
    EXPECT_TRUE(codec.validate(token, claims.issuer, now).has_value());
    EXPECT_TRUE(codec.validate(token, claims.issuer, now + 60s).has_value());
    EXPECT_FALSE(codec.validate(token, claims.issuer, now + 61s).has_value());
}

TEST(IdentityTokenTest, EnforcesIssuedAtLeewayBoundary) {
    const auto now = at_time(1'700'000'000);
    IdentityTokenCodec codec(test_seed(), "test-kid");
    auto claims = test_claims();
    claims.issued_at = now;
    const auto token = codec.issue(claims);
    EXPECT_TRUE(codec.validate(token, claims.issuer, now).has_value());
    EXPECT_TRUE(codec.validate(token, claims.issuer, now - 60s).has_value());
    EXPECT_FALSE(codec.validate(token, claims.issuer, now - 61s).has_value());
}

TEST(IdentityTokenTest, JwksMatchesOkpShapeWithRfc8037VectorKey) {
    EXPECT_EQ(
        test_codec().jwks(),
        R"({"keys":[{"crv":"Ed25519","kid":"test-kid","kty":"OKP",)"
        R"("x":"11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"}]})");

    IdentityTokenCodec other_codec(test_seed(), "kid-a");
    EXPECT_EQ(
        other_codec.jwks(),
        R"({"keys":[{"crv":"Ed25519","kid":"kid-a","kty":"OKP",)"
        R"("x":"11qYAYKxCrfVS_7TyWQHOg7hcvPapiMlrwIaaPcHURo"}]})");
}

TEST(IdentityTokenTest, ParsesSeedHexStrictly) {
    EXPECT_EQ(parse_identity_seed_hex(rfc8037_a3_seed_hex), test_seed());
    EXPECT_EQ(
        parse_identity_seed_hex("9D61B19DEFFD5A60BA844AF492EC2CC4"
                                "4449C5697B326919703BAC031CAE7F60"),
        test_seed());
    EXPECT_THROW(
        static_cast<void>(parse_identity_seed_hex("9d61")),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(parse_identity_seed_hex(std::string(65, 'a'))),
        std::invalid_argument);
    EXPECT_THROW(
        static_cast<void>(parse_identity_seed_hex(std::string(64, 'z'))),
        std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::common
