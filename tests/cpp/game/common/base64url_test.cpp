#include "realmmesh/game/common/base64url.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace realm::game::common {
namespace {

std::vector<std::byte> as_bytes(std::string_view text) {
    std::vector<std::byte> bytes;
    bytes.reserve(text.size());
    for (const char value : text) {
        bytes.push_back(static_cast<std::byte>(value));
    }
    return bytes;
}

std::string as_string(const std::vector<std::byte>& bytes) {
    std::string text;
    text.reserve(bytes.size());
    for (const auto value : bytes) {
        text.push_back(static_cast<char>(value));
    }
    return text;
}

TEST(Base64UrlTest, EncodesRfc4648TestVectorsWithoutPadding) {
    EXPECT_EQ(base64url_encode(as_bytes("")), "");
    EXPECT_EQ(base64url_encode(as_bytes("f")), "Zg");
    EXPECT_EQ(base64url_encode(as_bytes("fo")), "Zm8");
    EXPECT_EQ(base64url_encode(as_bytes("foo")), "Zm9v");
    EXPECT_EQ(base64url_encode(as_bytes("foob")), "Zm9vYg");
    EXPECT_EQ(base64url_encode(as_bytes("fooba")), "Zm9vYmE");
    EXPECT_EQ(base64url_encode(as_bytes("foobar")), "Zm9vYmFy");
    // 标准字母表的 '+' '/' 在这里必须落到 '-' '_'。
    EXPECT_EQ(base64url_encode(as_bytes("\xfb\xff")), "-_8");
    EXPECT_EQ(base64url_encode(as_bytes("\xfb")), "-w");
}

TEST(Base64UrlTest, DecodesRfc4648TestVectors) {
    EXPECT_EQ(as_string(*base64url_decode("")), "");
    EXPECT_EQ(as_string(*base64url_decode("Zg")), "f");
    EXPECT_EQ(as_string(*base64url_decode("Zm8")), "fo");
    EXPECT_EQ(as_string(*base64url_decode("Zm9v")), "foo");
    EXPECT_EQ(as_string(*base64url_decode("Zm9vYg")), "foob");
    EXPECT_EQ(as_string(*base64url_decode("Zm9vYmE")), "fooba");
    EXPECT_EQ(as_string(*base64url_decode("Zm9vYmFy")), "foobar");
    EXPECT_EQ(as_string(*base64url_decode("-_8")), "\xfb\xff");
    EXPECT_EQ(as_string(*base64url_decode("-w")), "\xfb");
}

TEST(Base64UrlTest, RoundTripsEveryLength) {
    for (std::size_t length = 0; length <= 70; ++length) {
        std::vector<std::byte> bytes;
        bytes.reserve(length);
        for (std::size_t index = 0; index < length; ++index) {
            bytes.push_back(static_cast<std::byte>((index * 7U + 3U) & 0xffU));
        }
        const auto encoded = base64url_encode(bytes);
        EXPECT_EQ(encoded.size(), (length + 2) / 3 * 4 - (3 - length % 3) % 3)
            << "length " << length;
        EXPECT_EQ(encoded.find('='), std::string::npos);
        EXPECT_EQ(encoded.find('+'), std::string::npos);
        EXPECT_EQ(encoded.find('/'), std::string::npos);
        const auto decoded = base64url_decode(encoded);
        ASSERT_TRUE(decoded.has_value()) << "length " << length;
        EXPECT_EQ(*decoded, bytes) << "length " << length;
    }
}

TEST(Base64UrlTest, RejectsPaddingWhitespaceAndNonAlphabetCharacters) {
    EXPECT_FALSE(base64url_decode("Zg==").has_value());
    EXPECT_FALSE(base64url_decode("Zg=").has_value());
    EXPECT_FALSE(base64url_decode("Zg ").has_value());
    EXPECT_FALSE(base64url_decode("Zg\n").has_value());
    EXPECT_FALSE(base64url_decode("+/8").has_value());
    EXPECT_FALSE(base64url_decode("Zg!").has_value());
}

TEST(Base64UrlTest, RejectsImpossibleLengths) {
    EXPECT_FALSE(base64url_decode("Z").has_value());
    EXPECT_FALSE(base64url_decode("ZgXaZ").has_value());
}

TEST(Base64UrlTest, RejectsNonCanonicalRemainderBits) {
    // 余位必须为 0(RFC 4648 §3.5):"Zh" 尾部余 0001,"Zm9" 尾部余 01。
    EXPECT_FALSE(base64url_decode("Zh").has_value());
    EXPECT_FALSE(base64url_decode("Zm9").has_value());
}

}  // namespace
}  // namespace realm::game::common
