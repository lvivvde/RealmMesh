#include "realmmesh/network/http/http1_parser.hpp"
#include "realmmesh/network/http/http1_response.hpp"

#include <gtest/gtest.h>

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace realm::network {
namespace {

constexpr std::string_view get_request =
    "GET /v1/queue/progress HTTP/1.1\r\n"
    "Host: queue.realmmesh.example\r\n"
    "\r\n";

constexpr std::string_view post_request =
    "POST /v1/login/verify HTTP/1.1\r\n"
    "Host: login.realmmesh.example\r\n"
    "Content-Type: application/json\r\n"
    "Content-Length: 19\r\n"
    "\r\n"
    "{\"account\":\"edwin\"}";

[[nodiscard]] ByteBuffer make_buffer(std::string_view text) {
    ByteBuffer buffer;
    buffer.append(std::as_bytes(std::span{text}));
    return buffer;
}

TEST(Http1ParserTest, ParsesSimpleGetRequest) {
    Http1Parser parser;
    auto buffer = make_buffer(get_request);
    const auto result = parser.try_parse(buffer);
    ASSERT_EQ(result.status, Http1ParseStatus::RequestReady);
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->method, "GET");
    EXPECT_EQ(result.request->target, "/v1/queue/progress");
    ASSERT_NE(result.request->header("Host"), nullptr);
    EXPECT_EQ(*result.request->header("host"), "queue.realmmesh.example");
    EXPECT_TRUE(result.request->body.empty());
    EXPECT_TRUE(result.request->wants_keep_alive());
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ParserTest, ParsesPostRequestWithBody) {
    Http1Parser parser;
    auto buffer = make_buffer(post_request);
    const auto result = parser.try_parse(buffer);
    ASSERT_EQ(result.status, Http1ParseStatus::RequestReady);
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->method, "POST");
    EXPECT_EQ(result.request->body, R"({"account":"edwin"})");
    EXPECT_EQ(*result.request->header("Content-Type"), "application/json");
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ParserTest, AssemblesARequestFedByteByByte) {
    Http1Parser parser;
    ByteBuffer buffer;
    const auto text = std::string{post_request};
    std::optional<Http1ParseResult> result;
    for (std::size_t index = 0; index < text.size(); ++index) {
        buffer.append(std::as_bytes(std::span{text}.subspan(index, 1)));
        result = parser.try_parse(buffer);
        if (index + 1 < text.size()) {
            ASSERT_EQ(result->status, Http1ParseStatus::NeedMoreData);
            ASSERT_FALSE(result->request.has_value());
        }
    }
    ASSERT_EQ(result->status, Http1ParseStatus::RequestReady);
    ASSERT_TRUE(result->request.has_value());
    EXPECT_EQ(result->request->body, R"({"account":"edwin"})");
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ParserTest, ProducesTheSameRequestAcrossChunkBoundaries) {
    const std::string_view text = post_request;
    const std::string_view chunks[] = {
        text.substr(0, 7), text.substr(7, 40), text.substr(47)};
    Http1Parser parser;
    ByteBuffer buffer;
    std::optional<Http1ParseResult> result;
    for (const auto chunk : chunks) {
        buffer.append(
            std::as_bytes(std::span{chunk.data(), chunk.size()}));
        result = parser.try_parse(buffer);
    }
    ASSERT_EQ(result->status, Http1ParseStatus::RequestReady);
    ASSERT_TRUE(result->request.has_value());
    EXPECT_EQ(result->request->method, "POST");
    EXPECT_EQ(result->request->target, "/v1/login/verify");
    EXPECT_EQ(result->request->body, R"({"account":"edwin"})");
}

TEST(Http1ParserTest, ParsesPipelinedRequestsSequentially) {
    const auto pipelined = std::string{get_request} + std::string{post_request};
    Http1Parser parser;
    auto buffer = make_buffer(pipelined);

    const auto first = parser.try_parse(buffer);
    ASSERT_EQ(first.status, Http1ParseStatus::RequestReady);
    ASSERT_TRUE(first.request.has_value());
    EXPECT_EQ(first.request->method, "GET");
    EXPECT_EQ(buffer.readable_bytes(), std::string{post_request}.size());

    const auto second = parser.try_parse(buffer);
    ASSERT_EQ(second.status, Http1ParseStatus::RequestReady);
    ASSERT_TRUE(second.request.has_value());
    EXPECT_EQ(second.request->method, "POST");
    EXPECT_TRUE(buffer.empty());
}

TEST(Http1ParserTest, NormalizesHeaderNamesToLowercaseForLookup) {
    Http1Parser parser;
    auto buffer = make_buffer(
        "GET / HTTP/1.1\r\n"
        "HOST: upper.example\r\n"
        "X-Custom-Id: 42\r\n"
        "\r\n");
    const auto result = parser.try_parse(buffer);
    ASSERT_EQ(result.status, Http1ParseStatus::RequestReady);
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(*result.request->header("host"), "upper.example");
    EXPECT_EQ(*result.request->header("x-custom-id"), "42");
    EXPECT_EQ(*result.request->header("X-CUSTOM-ID"), "42");
    EXPECT_EQ(result.request->header("missing"), nullptr);
}

TEST(Http1ParserTest, HonorsConnectionClose) {
    Http1Parser parser;
    auto closing = make_buffer(
        "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n");
    const auto closed = parser.try_parse(closing);
    ASSERT_TRUE(closed.request.has_value());
    EXPECT_FALSE(closed.request->wants_keep_alive());

    auto kept = make_buffer(
        "GET / HTTP/1.1\r\nHost: h\r\nConnection: keep-alive\r\n\r\n");
    const auto kept_result = parser.try_parse(kept);
    ASSERT_TRUE(kept_result.request.has_value());
    EXPECT_TRUE(kept_result.request->wants_keep_alive());

    auto mixed = make_buffer(
        "GET / HTTP/1.1\r\nHost: h\r\nConnection: keep-alive, upgrade\r\n\r\n");
    const auto mixed_result = parser.try_parse(mixed);
    ASSERT_TRUE(mixed_result.request.has_value());
    EXPECT_TRUE(mixed_result.request->wants_keep_alive());
}

TEST(Http1ParserTest, RejectsProtocolViolationsDeterministically) {
    struct RejectCase {
        std::string name;
        std::string raw;
        Http1ParseStatus status;
    };
    const RejectCase cases[] = {
        {"transfer_encoding",
         "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"transfer_encoding_with_length",
         "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
         "Content-Length: 3\r\n\r\nabc",
         Http1ParseStatus::BadRequest},
        {"duplicate_content_length",
         "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n"
         "Content-Length: 3\r\n\r\nabc",
         Http1ParseStatus::BadRequest},
        {"missing_host",
         "GET / HTTP/1.1\r\nUser-Agent: t\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"duplicate_host",
         "GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"http10",
         "GET / HTTP/1.0\r\nHost: h\r\n\r\n",
         Http1ParseStatus::VersionNotSupported},
        {"http20",
         "GET / HTTP/2.0\r\nHost: h\r\n\r\n",
         Http1ParseStatus::VersionNotSupported},
        {"missing_space_in_request_line",
         "GET/\r\nHost: h\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"empty_target",
         "GET  HTTP/1.1\r\nHost: h\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"absolute_form_target",
         "GET http://h/x HTTP/1.1\r\nHost: h\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"bad_method_characters",
         "GE,T / HTTP/1.1\r\nHost: h\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"header_without_colon",
         "GET / HTTP/1.1\r\nHost: h\r\nBadHeader\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"space_before_colon",
         "GET / HTTP/1.1\r\nHost : h\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"obs_folded_header",
         "GET / HTTP/1.1\r\nHost: h\r\nX-A: 1\r\n 2\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"control_char_in_value",
         std::string("GET / HTTP/1.1\r\nHost: h\r\nX-B: a\x01") + "b\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"content_length_not_a_number",
         "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 1x\r\n\r\n",
         Http1ParseStatus::BadRequest},
        {"content_length_negative",
         "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: -1\r\n\r\n",
         Http1ParseStatus::BadRequest},
    };
    for (const auto& test_case : cases) {
        SCOPED_TRACE(test_case.name);
        for (int attempt = 0; attempt < 2; ++attempt) {
            Http1Parser parser;
            auto buffer = make_buffer(test_case.raw);
            const auto result = parser.try_parse(buffer);
            EXPECT_EQ(result.status, test_case.status);
            EXPECT_FALSE(result.request.has_value());
        }
    }
}

TEST(Http1ParserTest, RejectsOversizedBodyBeforeItArrives) {
    Http1Parser parser(1024, 64);
    auto buffer = make_buffer(
        "POST / HTTP/1.1\r\nHost: h\r\nContent-Length: 65\r\n\r\n");
    EXPECT_EQ(
        parser.try_parse(buffer).status, Http1ParseStatus::PayloadTooLarge);
}

TEST(Http1ParserTest, RejectsOversizedHeadBlocks) {
    const auto padded = std::string("GET / HTTP/1.1\r\nHost: h\r\nX-Pad: ") +
        std::string(200, 'a') + "\r\n\r\n";
    Http1Parser parser(64, 1024);
    auto complete = make_buffer(padded);
    EXPECT_EQ(
        parser.try_parse(complete).status, Http1ParseStatus::HeadersTooLarge);

    // 不完整但已超限的头部同样即时拒绝——慢速发送不能绕开上限。
    const auto partial = std::string("GET / HTTP/1.1\r\n") + std::string(100, 'x');
    Http1Parser early_parser(64, 1024);
    auto incomplete = make_buffer(partial);
    EXPECT_EQ(
        early_parser.try_parse(incomplete).status,
        Http1ParseStatus::HeadersTooLarge);
}

TEST(Http1ParserTest, ReportsMaxInputBound) {
    EXPECT_EQ(Http1Parser(512, 1024).max_input_bytes(), 1536U);
    EXPECT_EQ(
        Http1Parser().max_input_bytes(),
        Http1Parser::default_max_head_bytes +
            Http1Parser::default_max_body_bytes);
}

/// 头预算只覆盖终止符之前;头 + 超过头预算的合法 body 同批到达
/// 必须照常解析(服务边一次事件会倾空全部可读字节,结论不得随
/// 分块方式变化)。
TEST(Http1ParserTest, ParsesCoDeliveredHeadAndLargeBody) {
    const std::string body(10 * 1024, 'x');
    const auto wire = std::string(
                          "POST /v1/login/verify HTTP/1.1\r\n"
                          "Host: login.realmmesh.example\r\n"
                          "Content-Length: 10240\r\n"
                          "\r\n") +
        body;
    ASSERT_GT(wire.size(), Http1Parser::default_max_head_bytes);

    Http1Parser parser;
    auto buffer = make_buffer(wire);
    const auto result = parser.try_parse(buffer);
    ASSERT_EQ(result.status, Http1ParseStatus::RequestReady);
    ASSERT_TRUE(result.request.has_value());
    EXPECT_EQ(result.request->body, body);
    EXPECT_TRUE(buffer.empty());

    // 逐字节到达的同一序列给出同一结论。
    Http1Parser byte_wise_parser;
    ByteBuffer byte_wise;
    std::optional<Http1ParseResult> fed;
    const auto bytes = std::as_bytes(std::span{wire});
    for (std::size_t index = 0; index < bytes.size(); ++index) {
        byte_wise.append(bytes.subspan(index, 1));
        fed = byte_wise_parser.try_parse(byte_wise);
    }
    ASSERT_EQ(fed->status, Http1ParseStatus::RequestReady);
    EXPECT_EQ(fed->request->body, body);
}

/// 拒绝后同一解析器再喂来的字节不得产生第二个请求(规格测试决策)。
TEST(Http1ParserTest, AfterRejectionSubsequentBytesProduceNoSecondRequest) {
    Http1Parser parser;
    auto buffer = make_buffer("GET / HTTP/1.1\r\nUser-Agent: t\r\n\r\n");
    const auto first = parser.try_parse(buffer);
    EXPECT_EQ(first.status, Http1ParseStatus::BadRequest);
    EXPECT_FALSE(first.request.has_value());

    buffer.append(std::as_bytes(std::span{get_request}));
    const auto second = parser.try_parse(buffer);
    EXPECT_EQ(second.status, Http1ParseStatus::BadRequest);
    EXPECT_FALSE(second.request.has_value());
}

TEST(Http1ResponseTest, SerializesStatusHeadersAndBody) {
    const Http1Response response{
        .status = 200,
        .headers = {{"Content-Type", "application/json"}},
        .body = "{}",
    };
    EXPECT_EQ(
        serialize_http1_response(response, true),
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: 2\r\n"
        "Connection: keep-alive\r\n"
        "\r\n"
        "{}");
}

TEST(Http1ResponseTest, WritesCloseAndEmptyBody) {
    const Http1Response response{
        .status = 202,
        .headers = {},
        .body = "",
    };
    EXPECT_EQ(
        serialize_http1_response(response, false),
        "HTTP/1.1 202 Accepted\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "\r\n");
}

TEST(Http1ResponseTest, UsesReasonPhraseTable) {
    const auto status_line = [](int status) {
        const auto serialized =
            serialize_http1_response(Http1Response{.status = status}, true);
        return serialized.substr(0, serialized.find("\r\n"));
    };
    EXPECT_EQ(status_line(200), "HTTP/1.1 200 OK");
    EXPECT_EQ(status_line(202), "HTTP/1.1 202 Accepted");
    EXPECT_EQ(status_line(400), "HTTP/1.1 400 Bad Request");
    EXPECT_EQ(status_line(413), "HTTP/1.1 413 Content Too Large");
    EXPECT_EQ(status_line(431), "HTTP/1.1 431 Request Header Fields Too Large");
    EXPECT_EQ(status_line(505), "HTTP/1.1 505 HTTP Version Not Supported");
    EXPECT_EQ(status_line(599), "HTTP/1.1 599");
}

}  // namespace
}  // namespace realm::network
