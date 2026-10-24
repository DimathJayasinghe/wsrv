// SPDX-License-Identifier: Apache-2.0
// End to end, still without sockets: raw bytes -> HttpRequestParser -> HandshakeNegotiator ->
// response text.
#include "base/bytes.hpp"
#include "http/request_parser.hpp"
#include "protocol/handshake.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <variant>

namespace {

constexpr std::string_view kBrowserRequest =
    "GET /chat HTTP/1.1\r\n"
    "Host: example.com\r\n"
    "Connection: Upgrade\r\n"
    "Upgrade: websocket\r\n"
    "Origin: https://app.example.com\r\n"
    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
    "Sec-WebSocket-Version: 13\r\n"
    "Sec-WebSocket-Protocol: chat, superchat\r\n"
    "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n"
    "\r\n";

wsrv::HandshakeConfig config() {
    wsrv::HandshakeConfig c;
    c.allowed_origins = {"https://app.example.com"};
    c.subprotocols = {"superchat"};
    return c;
}

TEST(HandshakeFlow, ABrowserRequestArrivingInThreeChunksWithTwoFrameBytesBehindIt) {
    // The network delivers the head in pieces, and the first two bytes of a WebSocket frame ride
    // along.
    const std::string stream = std::string{kBrowserRequest} + "\x81\x85";
    const std::size_t cut1 = 17;
    const std::size_t cut2 = 140;
    ASSERT_LT(cut2, kBrowserRequest.size());

    wsrv::HttpRequestParser parser;
    std::size_t consumed = 0;
    bool complete = false;
    for (const std::string_view piece : {std::string_view{stream}.substr(0, cut1),
                                         std::string_view{stream}.substr(cut1, cut2 - cut1),
                                         std::string_view{stream}.substr(cut2)}) {
        const auto progress = parser.feed(wsrv::as_bytes(piece));
        ASSERT_TRUE(progress.has_value());
        consumed += progress->consumed;
        complete = progress->complete;
    }
    ASSERT_TRUE(complete);
    EXPECT_EQ(consumed, kBrowserRequest.size());  // the 2 frame bytes were left alone...
    EXPECT_EQ(stream.size() - consumed, 2U);      // ...for the frame parser of the next milestone

    const wsrv::HandshakeDecision decision =
        wsrv::HandshakeNegotiator{config()}.negotiate(parser.request());
    const auto text = wsrv::serialize(wsrv::to_response(decision));
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text,
              "HTTP/1.1 101 Switching Protocols\r\n"
              "Upgrade: websocket\r\n"
              "Connection: Upgrade\r\n"
              "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
              "Sec-WebSocket-Protocol: superchat\r\n"
              "\r\n");
}

TEST(HandshakeFlow, ASmugglingAttemptIsAnsweredWith400AndClose) {
    std::string request{kBrowserRequest};
    request.insert(request.size() - 2, "Content-Length: 12\r\n");  // an upgrade request with a body
    wsrv::HttpRequestParser parser;
    const auto progress = parser.feed(wsrv::as_bytes(request));
    ASSERT_TRUE(progress.has_value());
    ASSERT_TRUE(progress->complete);
    const auto text = wsrv::serialize(
        wsrv::to_response(wsrv::HandshakeNegotiator{config()}.negotiate(parser.request())));
    ASSERT_TRUE(text.has_value());
    EXPECT_EQ(*text, "HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Length: 0\r\n\r\n");
}

TEST(HandshakeFlow, ParserErrorsMapToTheirStatusLines) {
    struct Case {
        std::string_view request;
        int status;
    };
    for (const Case& c :
         {Case{"GET / HTTP/1.1\nHost: x\r\n\r\n", 400}, Case{"GET / HTTP/2.0\r\n\r\n", 505}}) {
        wsrv::HttpRequestParser parser;
        const auto progress = parser.feed(wsrv::as_bytes(c.request));
        ASSERT_FALSE(progress.has_value());
        EXPECT_EQ(progress.error().status, c.status);
        wsrv::HttpResponse response;
        response.status = progress.error().status;
        response.headers = {{"Connection", "close"}, {"Content-Length", "0"}};
        EXPECT_TRUE(wsrv::serialize(response).has_value());
    }
}

}  // namespace
