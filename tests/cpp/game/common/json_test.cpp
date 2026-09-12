#include "realmmesh/game/common/json.hpp"

#include <gtest/gtest.h>

#include <string>

namespace realm::game::common {
namespace {

JsonObject make_claims() {
    return JsonObject{
        {"iss", JsonValue{std::string("https://login.realmmesh.example")}},
        {"sub", JsonValue{std::string("12345678901234567890")}},
        {"aud", JsonValue{std::string("realmmesh-access")}},
        {"purpose", JsonValue{std::string("access")}},
        {"iat", JsonValue{std::int64_t{1'700'000'000}}},
        {"exp", JsonValue{std::int64_t{1'700'001'800}}},
        {"jti", JsonValue{std::string("0f1e2d3c4b5a69788796a5b4c3d2e1f0")}},
    };
}

TEST(JsonCodecTest, EncodesSortedKeysInCompactForm) {
    const JsonObject object{
        {"zeta", JsonValue{std::int64_t{1}}},
        {"alpha", JsonValue{std::string("x")}},
        {"mu", JsonValue{true}},
        {"beta", JsonValue{false}},
    };
    EXPECT_EQ(
        JsonCodec::encode(object),
        R"json({"alpha":"x","beta":false,"mu":true,"zeta":1})json");
}

TEST(JsonCodecTest, EncodesEmptyObject) {
    EXPECT_EQ(JsonCodec::encode(JsonObject{}), "{}");
}

TEST(JsonCodecTest, EscapesQuotesBackslashesAndControlCharacters) {
    const JsonObject object{
        {"quote", JsonValue{std::string("a\"b")}},
        {"slash", JsonValue{std::string("a\\b")}},
        {"line", JsonValue{std::string("a\nb\tc")}},
        {"ctrl", JsonValue{std::string(1, '\x01')}},
    };
    EXPECT_EQ(
        JsonCodec::encode(object),
        R"json({"ctrl":"\u0001","line":"a\nb\tc","quote":"a\"b","slash":"a\\b"})json");
}

TEST(JsonCodecTest, PassesUtf8ThroughUntouched) {
    const JsonObject object{{"note", JsonValue{std::string("洪峰登录")}}};
    const auto encoded = JsonCodec::encode(object);
    EXPECT_EQ(encoded, R"json({"note":"洪峰登录"})json");
    const auto decoded = JsonCodec::decode(encoded);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(std::get<std::string>(decoded->at("note")), "洪峰登录");
}

TEST(JsonCodecTest, RoundTripsClaimsShape) {
    const auto claims = make_claims();
    const auto decoded = JsonCodec::decode(JsonCodec::encode(claims));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, claims);
}

TEST(JsonCodecTest, RoundTripsInt64Boundary) {
    const JsonObject object{
        {"max", JsonValue{std::int64_t{9'223'372'036'854'775'807}}},
        {"min", JsonValue{std::int64_t{-9'223'372'036'854'775'807 - 1}}},
    };
    const auto decoded = JsonCodec::decode(JsonCodec::encode(object));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, object);
}

TEST(JsonCodecTest, DecodesEmptyObject) {
    const auto decoded = JsonCodec::decode("{}");
    ASSERT_TRUE(decoded.has_value());
    EXPECT_TRUE(decoded->empty());
    EXPECT_TRUE(JsonCodec::decode("{} ").has_value());
    EXPECT_TRUE(JsonCodec::decode("{ }").has_value());
    EXPECT_FALSE(JsonCodec::decode("{}x").has_value());
}

TEST(JsonCodecTest, DecodesToleratesWhitespaceBetweenTokens) {
    const auto decoded =
        JsonCodec::decode("{ \"a\" : 1 ,\n\t\"b\" : \"x\" , \"c\": false }\r\n");
    ASSERT_TRUE(decoded.has_value());
    const JsonObject expected{
        {"a", JsonValue{std::int64_t{1}}},
        {"b", JsonValue{std::string("x")}},
        {"c", JsonValue{false}},
    };
    EXPECT_EQ(*decoded, expected);
}

TEST(JsonCodecTest, DecodesEscapesItEmits) {
    const JsonObject object{
        {"quote", JsonValue{std::string("a\"b")}},
        {"slash", JsonValue{std::string("a\\b")}},
        {"line", JsonValue{std::string("a\nb\tc")}},
        {"ctrl", JsonValue{std::string(1, '\x01')}},
    };
    const auto decoded = JsonCodec::decode(JsonCodec::encode(object));
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, object);
}

TEST(JsonCodecTest, RejectsNonObjectRoots) {
    EXPECT_FALSE(JsonCodec::decode("").has_value());
    EXPECT_FALSE(JsonCodec::decode("[]").has_value());
    EXPECT_FALSE(JsonCodec::decode("null").has_value());
    EXPECT_FALSE(JsonCodec::decode("123").has_value());
    EXPECT_FALSE(JsonCodec::decode("\"x\"").has_value());
    EXPECT_FALSE(JsonCodec::decode("true").has_value());
}

TEST(JsonCodecTest, RejectsNestedContainersAndNullValues) {
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":{}})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":{"b":1}})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":[1]})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":null})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":[]})json").has_value());
}

TEST(JsonCodecTest, RejectsFloatsAndMalformedNumbers) {
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":1.5})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":1e3})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":1E3})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":01})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":+1})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":-})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":- 1})json").has_value());
    EXPECT_FALSE(
        JsonCodec::decode(R"json({"a":9223372036854775808})json").has_value());
    EXPECT_FALSE(
        JsonCodec::decode(R"json({"a":-9223372036854775809})json").has_value());
}

TEST(JsonCodecTest, RejectsDuplicateKeys) {
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":1,"a":2})json").has_value());
}

TEST(JsonCodecTest, RejectsMalformedStrings) {
    EXPECT_FALSE(JsonCodec::decode(R"json({"a")json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a"})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":)json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":"})json").has_value());
    // 未转义的原始控制字符。
    const std::string raw_control = "{\"a\":\"" + std::string(1, '\x01') + "\"}";
    EXPECT_FALSE(JsonCodec::decode(raw_control).has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":"\q"})json").has_value());
    // `\/` 不在编码产出集内。
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":"\/"})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":"\u12"})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":"\u12G4"})json").has_value());
    // 大写十六进制与代理区码位:编码器从不产出,解码拒绝。
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":"\uD83D"})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":"\ud800"})json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":"\udc00"})json").has_value());
}

TEST(JsonCodecTest, RejectsTrailingContentAfterObject) {
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":1}x)json").has_value());
    EXPECT_FALSE(JsonCodec::decode(R"json({"a":1} {})json").has_value());
}

TEST(JsonCodecTest, EnforcesInputSizeBound) {
    const auto encoded = JsonCodec::encode(make_claims());
    ASSERT_GT(encoded.size(), 16U);
    ASSERT_TRUE(
        JsonCodec::decode(encoded, JsonCodec::default_max_input_size)
            .has_value());
    EXPECT_FALSE(JsonCodec::decode(encoded, encoded.size() - 1).has_value());
}

}  // namespace
}  // namespace realm::game::common
