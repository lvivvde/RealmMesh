#include "realmmesh/game/common/compact_jws.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

#include "realmmesh/game/common/base64url.hpp"
#include "realmmesh/game/common/json.hpp"

namespace realm::game::common {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view rfc8037_a3_seed_hex =
    "9d61b19deffd5a60ba844af492ec2cc44449c5697b326919703bac031cae7f60";

Ed25519Seed test_seed() {
    return parse_identity_seed_hex(rfc8037_a3_seed_hex);
}

CompactJws test_jws() {
    return CompactJws(test_seed());
}

JsonObject test_header() {
    return JsonObject{
        {"alg", std::string{"EdDSA"}},
        {"typ", std::string{"JWT"}},
        {"kid", std::string{"test-kid"}},
    };
}

JsonObject test_payload() {
    return JsonObject{
        {"iss", std::string{"realmmesh/test"}},
        {"number", std::int64_t{42}},
    };
}

TEST(CompactJwsTest, EncodeProducesCompactThreeSegments) {
    const auto token = test_jws().encode(test_header(), test_payload());
    EXPECT_EQ(std::count(token.begin(), token.end(), '.'), 2);
    EXPECT_TRUE(token.find('=') == std::string::npos);
    EXPECT_TRUE(token.find('+') == std::string::npos);
    EXPECT_TRUE(token.find('/') == std::string::npos);
}

TEST(CompactJwsTest, DecodeRoundTripsVerifiedPayload) {
    const CompactJws jws = test_jws();
    const auto token = jws.encode(test_header(), test_payload());
    const auto payload = jws.decode(token, "test-kid");
    ASSERT_TRUE(payload.has_value());
    EXPECT_EQ(*payload, test_payload());
}

TEST(CompactJwsTest, DecodeRejectsWrongKid) {
    const auto token = test_jws().encode(test_header(), test_payload());
    EXPECT_FALSE(test_jws().decode(token, "other-kid").has_value());
}

TEST(CompactJwsTest, DecodeRejectsTamperedPayload) {
    const auto token = test_jws().encode(test_header(), test_payload());
    const auto separator = token.rfind('.');
    const std::string signed_part = token.substr(0, separator);
    const auto payload = base64url_encode(std::as_bytes(std::span{
        std::string_view{R"({"iss":"realmmesh/test","number":43})"}}));
    const std::string forged = signed_part + "." + payload +
        token.substr(separator);
    EXPECT_FALSE(test_jws().decode(forged, "test-kid").has_value());
}

TEST(CompactJwsTest, DecodeRejectsForeignKey) {
    const CompactJws foreign(Ed25519Seed{});
    const auto token = test_jws().encode(test_header(), test_payload());
    EXPECT_FALSE(foreign.decode(token, "test-kid").has_value());
}

TEST(CompactJwsTest, DecodeRejectsMalformedSegmentCount) {
    const CompactJws jws = test_jws();
    const auto token = jws.encode(test_header(), test_payload());
    const auto first = token.find('.');
    EXPECT_FALSE(jws.decode(token.substr(first + 1), "test-kid").has_value());
    EXPECT_FALSE(jws.decode(token + ".extra", "test-kid").has_value());
    EXPECT_FALSE(jws.decode("not-a-token", "test-kid").has_value());
}

TEST(CompactJwsTest, DecodeRejectsHeaderSchemaViolations) {
    const CompactJws jws = test_jws();
    const JsonObject payload = test_payload();

    JsonObject missing_typ = test_header();
    static_cast<void>(missing_typ.erase("typ"));
    EXPECT_FALSE(
        jws.decode(jws.encode(missing_typ, payload), "test-kid").has_value());

    JsonObject extra_member = test_header();
    extra_member.emplace("crit", JsonValue(std::string{"[]"}));
    EXPECT_FALSE(
        jws.decode(jws.encode(extra_member, payload), "test-kid").has_value());

    JsonObject wrong_alg = test_header();
    wrong_alg["alg"] = JsonValue(std::string{"HS256"});
    EXPECT_FALSE(
        jws.decode(jws.encode(wrong_alg, payload), "test-kid").has_value());
}

TEST(CompactJwsTest, ClockLeewayIsSharedConstant) {
    EXPECT_EQ(jws_clock_leeway, 60s);
}

TEST(CompactJwsTest, ParseSeedRejectsBadHex) {
    EXPECT_THROW(
        parse_identity_seed_hex("nothex"), std::invalid_argument);
    EXPECT_THROW(
        parse_identity_seed_hex("9d61"), std::invalid_argument);
}

}  // namespace
}  // namespace realm::game::common
