// SPDX-License-Identifier: Apache-2.0
// The byte form of a waiting upgrade inside a HostEvent (ADR-0018): one encoding, read by the C
// layer and by wsrvd, so the two cannot drift apart.
#include "capi/handshake_blob.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

wsrv::HandshakeRequest sample() {
    wsrv::HandshakeRequest request;
    request.id = 42;
    request.path = "/room/7";
    request.query = "token=abc";
    request.origin = "https://app.example.com";
    request.host = "example.com";
    request.subprotocol = "chat";
    request.offered_subprotocols = {"chat", "v2"};
    request.forwarded = {{"Cookie", "a=1"}, {"Cookie", "b=2"}, {"Authorization", "Bearer t"}};
    return request;
}

TEST(HandshakeBlob, RoundTripsEveryField) {
    const std::vector<std::byte> blob = wsrv::encode_handshake("10.0.0.1:5000", sample());
    const auto view = wsrv::decode_handshake(wsrv::ByteView{blob});
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->peer, "10.0.0.1:5000");
    EXPECT_EQ(view->path, "/room/7");
    EXPECT_EQ(view->query, "token=abc");
    EXPECT_EQ(view->origin, "https://app.example.com");
    EXPECT_EQ(view->host, "example.com");
    EXPECT_EQ(view->subprotocol, "chat");
    ASSERT_EQ(view->offered.size(), 2U);
    EXPECT_EQ(view->offered[1], "v2");
    ASSERT_EQ(view->headers.size(), 3U);
    EXPECT_EQ(view->headers[0].first, "Cookie");
    EXPECT_EQ(view->headers[2].second, "Bearer t");
}

TEST(HandshakeBlob, EmptyFieldsAndEmptyListsRoundTrip) {
    wsrv::HandshakeRequest empty;
    const auto blob = wsrv::encode_handshake("", empty);
    const auto view = wsrv::decode_handshake(wsrv::ByteView{blob});
    ASSERT_TRUE(view.has_value());
    EXPECT_TRUE(view->path.empty());
    EXPECT_TRUE(view->offered.empty());
    EXPECT_TRUE(view->headers.empty());
}

TEST(HandshakeBlob, ValuesWithAnyBytesSurviveBecauseLengthsNotSeparatorsDelimitThem) {
    wsrv::HandshakeRequest request = sample();
    request.forwarded = {{"X-Bin", std::string("a\0b\r\n,c", 7)}};
    const auto blob = wsrv::encode_handshake("p", request);
    const auto view = wsrv::decode_handshake(wsrv::ByteView{blob});
    ASSERT_TRUE(view.has_value());
    EXPECT_EQ(view->headers[0].second, std::string("a\0b\r\n,c", 7));
}

TEST(HandshakeBlob, EveryTruncationIsRefusedNeverReadPastTheEnd) {
    const auto blob = wsrv::encode_handshake("10.0.0.1:5000", sample());
    for (std::size_t size = 0; size < blob.size(); ++size) {
        const wsrv::ByteView cut{blob.data(), size};
        EXPECT_FALSE(wsrv::decode_handshake(cut).has_value()) << "cut at " << size;
    }
}

TEST(HandshakeBlob, ADeclaredLengthBeyondTheBufferIsRefused) {
    auto blob = wsrv::encode_handshake("p", sample());
    blob[0] = std::byte{0x7F};  // the first string claims to be gigantic
    EXPECT_FALSE(wsrv::decode_handshake(wsrv::ByteView{blob}).has_value());
}

TEST(HandshakeBlob, TrailingBytesAreRefused) {
    auto blob = wsrv::encode_handshake("p", sample());
    blob.push_back(std::byte{0});
    EXPECT_FALSE(wsrv::decode_handshake(wsrv::ByteView{blob}).has_value());
}

}  // namespace
