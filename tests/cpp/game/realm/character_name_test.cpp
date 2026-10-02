#include "realmmesh/game/realm/character_name.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using realm::game::realm::is_valid_character_name;

TEST(CharacterNameTest, AcceptsOneToSixteenCodePoints) {
    EXPECT_TRUE(is_valid_character_name("A"));
    EXPECT_TRUE(is_valid_character_name("Ranger"));
    EXPECT_TRUE(is_valid_character_name("abcdefghijklmnop"));
    EXPECT_FALSE(is_valid_character_name("abcdefghijklmnopq"));
    EXPECT_FALSE(is_valid_character_name(""));
}

TEST(CharacterNameTest, CountsCodePointsNotBytes) {
    // 16 个汉字是 48 字节,仍合法;17 个不合法。
    EXPECT_TRUE(is_valid_character_name(
        "一二三四五六七八九十甲乙丙丁戊己"));
    EXPECT_FALSE(is_valid_character_name(
        "一二三四五六七八九十甲乙丙丁戊己庚"));
    EXPECT_TRUE(is_valid_character_name("\xF0\x9F\x90\x89 dragon"));
}

TEST(CharacterNameTest, AllowsInnerWhitespaceButNotAtEitherEnd) {
    EXPECT_TRUE(is_valid_character_name("Sir Lancelot"));
    EXPECT_FALSE(is_valid_character_name(" Lancelot"));
    EXPECT_FALSE(is_valid_character_name("Lancelot "));
    // U+3000 全角空格、U+00A0 不换行空格同样是 Unicode 空白。
    EXPECT_FALSE(is_valid_character_name("\xE3\x80\x80name"));
    EXPECT_FALSE(is_valid_character_name("name\xC2\xA0"));
    EXPECT_TRUE(is_valid_character_name("a\xE3\x80\x80" "b"));
}

TEST(CharacterNameTest, RejectsControlCharacters) {
    EXPECT_FALSE(is_valid_character_name(std::string("a\0b", 3)));
    EXPECT_FALSE(is_valid_character_name("a\tb"));
    EXPECT_FALSE(is_valid_character_name("a\nb"));
    EXPECT_FALSE(is_valid_character_name("a\x7F" "b"));
    // C1:U+0085、U+009B。
    EXPECT_FALSE(is_valid_character_name("a\xC2\x85" "b"));
    EXPECT_FALSE(is_valid_character_name("a\xC2\x9B" "b"));
}

TEST(CharacterNameTest, RejectsMalformedUtf8) {
    EXPECT_FALSE(is_valid_character_name("\xFF"));
    EXPECT_FALSE(is_valid_character_name("a\xC3"));
    // 过长编码的 '/'、UTF-16 代理区、超出 U+10FFFF。
    EXPECT_FALSE(is_valid_character_name("\xC0\xAF"));
    EXPECT_FALSE(is_valid_character_name("\xED\xA0\x80"));
    EXPECT_FALSE(is_valid_character_name("\xF4\x90\x80\x80"));
    EXPECT_FALSE(is_valid_character_name("\xE4\xB8"));
}

}  // namespace
