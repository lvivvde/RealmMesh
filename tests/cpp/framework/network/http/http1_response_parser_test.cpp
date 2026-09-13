#include "realmmesh/network/http/http1_response_parser.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace realm::network {
namespace {

constexpr std::string_view ok_response =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 17\r\n"
    "\r\n"
    "{\"queued\":\"true\"}";

constexpr std::string_view not_found_response =
    "HTTP/1.1 404 Not Found\r\n"
    "Content-Length: 0\r\n"
    "\r\n";

[[nodiscard]] ByteBuffer make_buffer(std::string_view text) {
    ByteBuffer buffer;
    buffer.append(std::as_bytes(std::span{text}));
    return buffer;
}

TEST(Http1ResponseParserTest, ParsesOkResponseWithBody) {
    Http1ResponseParser parser;
    auto buffer = make_buffer(ok_response);
    const auto result = parser.try_parse(buffer);
    ASSERT_EQ(result.status, Http1ResponseParseStatus::ResponseReady);
    ASSERT_TRUE(result.response.has_value());
    EXPECT_EQ(result.response->status, 200);
    EXPECT_EQ(result.response->body, R"({"queued":"true"})");
    ASSERT_NE(result.response->header("content-type"), nullptr);
    EXPECT_EQ(*result.response->header("Content-Type"),
              "application/json");
    EXPECT_EQ(*result.response->header("CONTENT-LENGTH"), "17");
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ResponseParserTest, ParsesStatusOnlyResponseWithReason) {
    Http1ResponseParser parser;
    auto buffer = make_buffer(not_found_response);
    const auto result = parser.try_parse(buffer);
    ASSERT_EQ(result.status, Http1ResponseParseStatus::ResponseReady);
    ASSERT_TRUE(result.response.has_value());
    EXPECT_EQ(result.response->status, 404);
    EXPECT_TRUE(result.response->body.empty());
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ResponseParserTest, AssemblesAResponseFedByteByByte) {
    Http1ResponseParser parser;
    ByteBuffer buffer;
    const auto text = std::string{ok_response};
    std::optional<Http1ResponseParseResult> result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        buffer.append(std::as_bytes(std::span{text}.subspan(index, 1)));
        result = parser.try_parse(buffer);
        if (index + 1 < text.size()) {
            ASSERT_EQ(result->status, Http1ResponseParseStatus::NeedMoreData);
            ASSERT_FALSE(result->response.has_value());
        }
    }
    ASSERT_EQ(result->status, Http1ResponseParseStatus::ResponseReady);
    ASSERT_TRUE(result->response.has_value());
    EXPECT_EQ(result->response->body, R"({"queued":"true"})");
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ResponseParserTest, TreatsMissingContentLengthAsEmptyBody) {
    Http1ResponseParser parser;
    auto buffer = make_buffer(
        "HTTP/1.1 204 No Content\r\n"
        "\r\n");
    const auto result = parser.try_parse(buffer);
    ASSERT_EQ(result.status, Http1ResponseParseStatus::ResponseReady);
    ASSERT_TRUE(result.response.has_value());
    EXPECT_EQ(result.response->status, 204);
    EXPECT_TRUE(result.response->body.empty());
}

TEST(Http1ResponseParserTest, LeavesPipelinedRemainderInBuffer) {
    const auto pipelined =
        std::string{ok_response} + std::string{not_found_response};
    Http1ResponseParser parser;
    auto buffer = make_buffer(pipelined);

    const auto first = parser.try_parse(buffer);
    ASSERT_EQ(first.status, Http1ResponseParseStatus::ResponseReady);
    ASSERT_TRUE(first.response.has_value());
    EXPECT_EQ(first.response->status, 200);
    EXPECT_EQ(buffer.readable_bytes(),
              std::string{not_found_response}.size());

    const auto second = parser.try_parse(buffer);
    ASSERT_EQ(second.status, Http1ResponseParseStatus::ResponseReady);
    ASSERT_TRUE(second.response.has_value());
    EXPECT_EQ(second.response->status, 404);
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ResponseParserTest, RejectsTransferEncoding) {
    Http1ResponseParser parser;
    auto buffer = make_buffer(
        "HTTP/1.1 200 OK\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "0\r\n\r\n");
    const auto result = parser.try_parse(buffer);
    EXPECT_EQ(result.status, Http1ResponseParseStatus::BadResponse);
    EXPECT_FALSE(result.response.has_value());
}

TEST(Http1ResponseParserTest, RejectsDuplicateContentLength) {
    Http1ResponseParser parser;
    auto buffer = make_buffer(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 5\r\n"
        "content-length: 6\r\n"
        "\r\n"
        "hello");
    const auto result = parser.try_parse(buffer);
    EXPECT_EQ(result.status, Http1ResponseParseStatus::BadResponse);
}

TEST(Http1ResponseParserTest, RejectsBodyBeyondLimit) {
    Http1ResponseParser parser{1024, 8};
    auto buffer = make_buffer(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 20\r\n"
        "\r\n"
        "0123456789abcdefghij");
    const auto result = parser.try_parse(buffer);
    EXPECT_EQ(result.status, Http1ResponseParseStatus::PayloadTooLarge);
}

TEST(Http1ResponseParserTest, RejectsHeadBeyondLimit) {
    Http1ResponseParser parser{16, 1024};
    auto buffer = make_buffer(
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 0\r\n"
        "X-Overflow: aaaaaaaaaaaaaaaaaaaa\r\n"
        "\r\n");
    const auto result = parser.try_parse(buffer);
    EXPECT_EQ(result.status, Http1ResponseParseStatus::HeadersTooLarge);
}

TEST(Http1ResponseParserTest, HoldsBodyUntilContentLengthSatisfied) {
    // Content-Length 截断专用用例:头 + 半截 body 只报 NeedMoreData 且
    // 不产出响应;补齐剩余字节后整段 body 一次到位。
    constexpr std::string_view head =
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 17\r\n"
        "\r\n";
    const auto text = std::string{head} + R"({"queued":"true"})";
    Http1ResponseParser parser;
    auto buffer = make_buffer(text.substr(0, head.size() + 5));
    auto result = parser.try_parse(buffer);
    EXPECT_EQ(result.status, Http1ResponseParseStatus::NeedMoreData);
    EXPECT_FALSE(result.response.has_value());
    EXPECT_TRUE(buffer.empty());

    buffer.append(
        std::as_bytes(std::span{text}.subspan(head.size() + 5)));
    result = parser.try_parse(buffer);
    ASSERT_EQ(result.status, Http1ResponseParseStatus::ResponseReady);
    ASSERT_TRUE(result.response.has_value());
    EXPECT_EQ(result.response->body, R"({"queued":"true"})");
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ResponseParserTest, SlowDripHeadStillHitsSizeLimit) {
    // 慢速投喂不绕开头块上限:逐字节喂、终止符始终缺席,累计字节一超
    // max_head_bytes 立即 HeadersTooLarge,而非无限 NeedMoreData。
    Http1ResponseParser parser{16, 1024};
    ByteBuffer buffer;
    std::optional<Http1ResponseParseResult> result;
    for (std::size_t sent = 0; sent < 17; ++sent) {
        buffer.append(std::as_bytes(std::span{
            "HTTP/1.1 200 OK\r\nX-Long: aaaa\r\n"}.subspan(sent, 1)));
        result = parser.try_parse(buffer);
        if (sent + 1 < 17) {
            ASSERT_EQ(result->status, Http1ResponseParseStatus::NeedMoreData);
        }
    }
    EXPECT_EQ(result->status, Http1ResponseParseStatus::HeadersTooLarge);
    EXPECT_FALSE(result->response.has_value());
}

TEST(Http1ResponseParserTest, RejectsHttp10Response) {
    Http1ResponseParser parser;
    auto buffer = make_buffer(
        "HTTP/1.0 200 OK\r\n"
        "Content-Length: 0\r\n"
        "\r\n");
    const auto result = parser.try_parse(buffer);
    EXPECT_EQ(result.status, Http1ResponseParseStatus::VersionNotSupported);
}

TEST(Http1ResponseParserTest, RejectsGarbageStatusLine) {
    Http1ResponseParser parser;
    auto buffer = make_buffer("not an http response at all\r\n\r\n");
    const auto result = parser.try_parse(buffer);
    EXPECT_EQ(result.status, Http1ResponseParseStatus::BadResponse);
}

TEST(Http1ResponseParserTest, HeaderLookupIsCaseInsensitiveOnStruct) {
    Http1Response response;
    response.headers.emplace_back("Retry-After", "7");
    ASSERT_NE(response.header("retry-after"), nullptr);
    EXPECT_EQ(*response.header("RETRY-AFTER"), "7");
    EXPECT_EQ(response.header("missing"), nullptr);
}

}  // namespace
}  // namespace realm::network
