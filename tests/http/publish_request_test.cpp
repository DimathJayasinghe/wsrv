// SPDX-License-Identifier: Apache-2.0
#include "http/publish_request.hpp"

#include "base/bytes.hpp"
#include "http/request_parser.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using wsrv::HttpRequestParser;

constexpr std::string_view kGood = "POST /publish?topic=room.7 HTTP/1.1\r\n"
                                   "Host: wsrvd:9200\r\n"
                                   "Authorization: Bearer s3cret\r\n"
                                   "Content-Length: 5\r\n"
                                   "\r\n";

/// Parse the head with the real head parser, then interpret it.
wsrv::HttpResult<wsrv::PublishRequest> run(std::string_view text) {
    HttpRequestParser parser;
    const auto progress = parser.feed(wsrv::as_bytes(text));
    if (!progress) {
        return std::unexpected{progress.error()};
    }
    EXPECT_TRUE(progress->complete) << text;
    return wsrv::interpret_publish(parser.request());
}

/// kGood with one line swapped or added, for the refusal tests.
std::string with(std::string_view needle, std::string_view replacement) {
    std::string text{kGood};
    const auto at = text.find(needle);
    EXPECT_NE(at, std::string::npos) << needle;
    text.replace(at, needle.size(), replacement);
    return text;
}

int status_of(std::string_view text) {
    const auto result = run(text);
    return result ? 0 : result.error().status;
}

TEST(PublishRequest, AGoodRequestIsReadAsWritten) {
    const auto result = run(kGood);
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->topic, "room.7");
    EXPECT_EQ(result->except, 0U);
    EXPECT_FALSE(result->binary);
    EXPECT_EQ(result->content_length, 5U);
    EXPECT_EQ(result->bearer, "s3cret");
}

TEST(PublishRequest, ExceptAndBinaryAreRead) {
    const auto result = run(with("topic=room.7", "topic=a&except=18446744073709551615&binary=1"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->except, 18446744073709551615ULL);
    EXPECT_TRUE(result->binary);
}

TEST(PublishRequest, TheSchemeNameIsCaseInsensitiveAndZeroLengthIsAllowed) {
    const auto result = run(with("Bearer s3cret", "bearer s3cret"));
    EXPECT_TRUE(result.has_value());
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 0")), 0);
}

TEST(PublishRequest, OnlyPostToExactlyPublishIsServed) {  // 405 before 404, both before auth
    EXPECT_EQ(status_of(with("POST ", "GET ")), 405);
    EXPECT_EQ(status_of(with("POST ", "post ")), 405);  // a method is case-sensitive (RFC 9110 9.1)
    EXPECT_EQ(status_of(with("/publish", "/publish/")), 404);
    EXPECT_EQ(status_of(with("/publish", "/Publish")), 404);
    EXPECT_EQ(status_of(with("/publish?", "/other?")), 404);
    EXPECT_EQ(status_of(with("/publish?topic=room.7", "/publish")), 400);  // no topic at all
}

TEST(PublishRequest, ACredentialIsRequiredOnceAndMustLookLikeBearer) {  // 401, never a hint why
    EXPECT_EQ(status_of(with("Authorization: Bearer s3cret\r\n", "")), 401);
    EXPECT_EQ(status_of(with("Authorization: Bearer s3cret\r\n",
                             "Authorization: Bearer a\r\nAuthorization: Bearer a\r\n")),
              401);
    EXPECT_EQ(status_of(with("Bearer s3cret", "Basic czNjcmV0")), 401);
    EXPECT_EQ(status_of(with("Bearer s3cret", "Bearer")), 401);
    EXPECT_EQ(status_of(with("Bearer s3cret", "Bearer ")), 401);
    EXPECT_EQ(status_of(with("Bearer s3cret", "Bearer  s3cret")), 401);  // two spaces
    EXPECT_EQ(status_of(with("Bearer s3cret", "Bearer s3 cret")), 401);  // inner space
    EXPECT_EQ(status_of(with("Bearer s3cret", "Bearers3cret")), 401);
}

TEST(PublishRequest, AnyTransferEncodingIsNotImplemented) {  // chunked and friends: 501
    const std::string chunked = with("Content-Length: 5", "Transfer-Encoding: chunked");
    EXPECT_EQ(status_of(chunked), 501);
    // Both framings at once is the classic smuggling shape: still refused, not "Content-Length
    // wins".
    EXPECT_EQ(
        status_of(with("Content-Length: 5", "Content-Length: 5\r\nTransfer-Encoding: chunked")),
        501);
    EXPECT_EQ(
        status_of(with("Content-Length: 5", "Content-Length: 5\r\nTransfer-Encoding: identity")),
        501);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 5\r\ntRaNsFeR-eNcOdInG: x")),
              501);
}

TEST(PublishRequest, ExpectIsRefused) {  // 417: we never send "100 Continue"
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 5\r\nExpect: 100-continue")),
              417);
}

TEST(PublishRequest, AContentLengthIsRequiredOnceAndMustBePlainDecimal) {
    EXPECT_EQ(status_of(with("Content-Length: 5\r\n", "")), 411);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 5\r\nContent-Length: 5")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 5\r\nContent-Length: 6")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 5, 5")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: -1")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: +5")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 0x5")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 5.0")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: ")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 05")), 400);  // canonical only
}

TEST(PublishRequest, ALengthThatOverflowsIsRefusedNotWrapped) {
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 99999999999999999999999")), 400);
    EXPECT_EQ(status_of(with("Content-Length: 5", "Content-Length: 18446744073709551616")), 400);
    // A big but representable length is the endpoint's to bound (413 after auth), not ours.
    const auto big = run(with("Content-Length: 5", "Content-Length: 4294967296"));
    ASSERT_TRUE(big.has_value());
    EXPECT_EQ(big->content_length, 4294967296U);
}

TEST(PublishRequest, TheQueryIsExactlyTopicExceptBinary) {
    EXPECT_EQ(status_of(with("topic=room.7", "topic=")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&topic=b")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&except=1&except=2")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&binary=1&binary=1")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&extra=1")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "Topic=a")), 400);  // names are case-sensitive
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&&binary=1")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&binary")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&binary=2")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&binary=true")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&except=")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&except=-1")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&except=1x")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&except=18446744073709551616")), 400);
}

TEST(PublishRequest, AValueHoldsNoSecondSeparator) {  // found by fuzz_publish_request
    EXPECT_EQ(status_of(with("topic=room.7", "topic=?")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a?b")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a=b")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&except=1=1")), 400);
}

TEST(PublishRequest, NoPercentDecodingAtAll) {  // no second opinion about what a topic spells
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a%2Fb")), 400);
    EXPECT_EQ(status_of(with("topic=room.7", "topic=a&binary=%31")), 400);
}

TEST(PublishRequest, TheTopicIsPassedOnRawForTheBrokerToJudge) {
    const auto result = run(with("topic=room.7", "topic=we!rd..name"));
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->topic, "we!rd..name");
}

TEST(PublishRequest, ABodyAfterTheHeadIsNotTheHeadParsersBusiness) {
    // The parser stops after the blank line; the interpreter only sees the head.
    EXPECT_EQ(status_of(std::string{kGood} + "hello"), 0);
}

}  // namespace
