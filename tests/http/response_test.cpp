// SPDX-License-Identifier: Apache-2.0
#include "http/response.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>

namespace {

using wsrv::ErrorCode;
using wsrv::HttpResponse;

TEST(HttpResponse, SerialisesTheSwitchingProtocolsResponseExactly) {
    HttpResponse response;
    response.status = 101;
    response.headers = {{"Upgrade", "websocket"},
                        {"Connection", "Upgrade"},
                        {"Sec-WebSocket-Accept", "s3pPLMBiTxaQ9kYGzzhZRbK+xOo="}};
    const auto text = wsrv::serialize(response);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text,
              "HTTP/1.1 101 Switching Protocols\r\n"
              "Upgrade: websocket\r\n"
              "Connection: Upgrade\r\n"
              "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
              "\r\n");
}

TEST(HttpResponse, AddsNothingImplicitlyAndWorksWithoutHeaders) {
    HttpResponse response;
    response.status = 400;
    const auto text = wsrv::serialize(response);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "HTTP/1.1 400 Bad Request\r\n\r\n");
}

TEST(HttpResponse, ReasonPhrasesForTheStatusesTheHandshakeUses) {
    EXPECT_EQ(wsrv::reason_phrase(101), "Switching Protocols");
    EXPECT_EQ(wsrv::reason_phrase(400), "Bad Request");
    EXPECT_EQ(wsrv::reason_phrase(403), "Forbidden");
    EXPECT_EQ(wsrv::reason_phrase(405), "Method Not Allowed");
    EXPECT_EQ(wsrv::reason_phrase(414), "URI Too Long");
    EXPECT_EQ(wsrv::reason_phrase(426), "Upgrade Required");
    EXPECT_EQ(wsrv::reason_phrase(429), "Too Many Requests");
    EXPECT_EQ(wsrv::reason_phrase(431), "Request Header Fields Too Large");
    EXPECT_EQ(wsrv::reason_phrase(500), "Internal Server Error");
    EXPECT_EQ(wsrv::reason_phrase(503), "Service Unavailable");
    EXPECT_EQ(wsrv::reason_phrase(505), "HTTP Version Not Supported");
    EXPECT_EQ(wsrv::reason_phrase(299), "");  // unknown: empty, still a legal status line
}

TEST(HttpResponse, UnknownButValidStatusStillSerialises) {
    HttpResponse response;
    response.status = 299;
    const auto text = wsrv::serialize(response);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "HTTP/1.1 299 \r\n\r\n");
}

TEST(HttpResponse, StatusOutsideTheThreeDigitRangeIsRejected) {
    for (const int status : {-1, 0, 99, 600, 1000}) {
        HttpResponse response;
        response.status = status;
        const auto text = wsrv::serialize(response);
        ASSERT_FALSE(text.has_value()) << status;
        EXPECT_EQ(text.error().code, ErrorCode::InvalidArgument) << status;
    }
}

TEST(HttpResponse, HeaderInjectionAttemptsAreRejected) {  // response splitting, SECURITY_DESIGN T9
    const std::array<std::string_view, 6> bad_values{
        std::string_view{"x\r\nSet-Cookie: stolen=1"},
        std::string_view{"x\nSet-Cookie: a"},
        std::string_view{"x\rSet-Cookie: a"},
        std::string_view{"nul\0byte", 8},
        std::string_view{"bell\a"},
        std::string_view{"del\x7f"},
    };
    for (const std::string_view value : bad_values) {
        HttpResponse response;
        response.status = 400;
        response.headers = {{"X-Test", std::string{value}}};
        const auto text = wsrv::serialize(response);
        ASSERT_FALSE(text.has_value()) << "accepted a value with a control character";
        EXPECT_EQ(text.error().code, ErrorCode::InvalidArgument);
    }
}

TEST(HttpResponse, InvalidHeaderNamesAreRejected) {
    for (const std::string_view name : {"", "Bad Name", "Bad:Name", "Bad\r\nName", "caf\xC3\xA9"}) {
        HttpResponse response;
        response.status = 400;
        response.headers = {{std::string{name}, "v"}};
        EXPECT_FALSE(wsrv::serialize(response).has_value()) << name;
    }
}

TEST(HttpResponse, TabsAndEmptyValuesAndObsTextAreFine) {
    HttpResponse response;
    response.status = 400;
    response.headers = {{"A", "with\ttab"}, {"B", ""}, {"C", "caf\xC3\xA9"}};
    const auto text = wsrv::serialize(response);
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "HTTP/1.1 400 Bad Request\r\nA: with\ttab\r\nB: \r\nC: caf\xC3\xA9\r\n\r\n");
}

}  // namespace
