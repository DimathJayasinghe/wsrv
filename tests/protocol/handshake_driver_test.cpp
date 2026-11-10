// SPDX-License-Identifier: Apache-2.0
#include "protocol/handshake_driver.hpp"

#include "base/bytes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <string_view>

namespace {

using wsrv::HandshakeDriver;
using wsrv::HandshakeNegotiator;
using wsrv::HandshakeStep;

constexpr std::string_view kRequest = "GET /chat HTTP/1.1\r\n"
                                      "Host: example.com\r\n"
                                      "Connection: Upgrade\r\n"
                                      "Upgrade: websocket\r\n"
                                      "Origin: https://app.example.com\r\n"
                                      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                      "Sec-WebSocket-Version: 13\r\n"
                                      "Sec-WebSocket-Protocol: chat\r\n"
                                      "\r\n";

constexpr std::string_view kAccepted = "HTTP/1.1 101 Switching Protocols\r\n"
                                       "Upgrade: websocket\r\n"
                                       "Connection: Upgrade\r\n"
                                       "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n"
                                       "Sec-WebSocket-Protocol: chat\r\n"
                                       "\r\n";

wsrv::HandshakeConfig handshake_config() {
    wsrv::HandshakeConfig config;
    config.allowed_origins = {"https://app.example.com"};
    config.subprotocols = {"chat"};
    return config;
}

struct HandshakeDriverTest : public ::testing::Test {
    HandshakeNegotiator negotiator{handshake_config()};
    HandshakeDriver driver{negotiator, wsrv::HttpLimits{}};

    wsrv::HandshakeOutcome feed(std::string_view chunk) {
        return driver.feed(wsrv::as_bytes(chunk));
    }
};

TEST_F(HandshakeDriverTest, AValidUpgradeIsAcceptedWithTheExactResponse) {
    const wsrv::HandshakeOutcome outcome = feed(kRequest);
    ASSERT_EQ(outcome.step, HandshakeStep::Accepted);
    EXPECT_EQ(outcome.response, kAccepted);
    EXPECT_EQ(outcome.consumed, kRequest.size());
    EXPECT_EQ(outcome.subprotocol, "chat");
}

TEST_F(HandshakeDriverTest,
       AnAcceptedOutcomeKeepsTheKeyAndTheRequestForALaterDecision) {  // ADR-0018
    const wsrv::HandshakeOutcome outcome = feed(kRequest);
    ASSERT_EQ(outcome.step, HandshakeStep::Accepted);
    EXPECT_EQ(outcome.accept_key, "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    EXPECT_EQ(driver.request().target, "/chat");
    EXPECT_EQ(driver.request().values("Origin").size(), 1U);
}

// ADR-0020: the connection builds the session's codecs from the agreed parameters, and renders the
// 101 again when the application picks a subprotocol later: both need the agreement, not only
// the header text it was rendered into.
TEST(HandshakeDriverDeflate, AnAcceptedOutcomeCarriesTheAgreement) {
    wsrv::HandshakeConfig config = handshake_config();
    config.permessage_deflate = wsrv::DeflatePolicy{};
    const HandshakeNegotiator negotiator{config};
    HandshakeDriver driver{negotiator, wsrv::HttpLimits{}};
    std::string request{kRequest};
    request.insert(request.size() - 2,
                   "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n");
    const wsrv::HandshakeOutcome outcome = driver.feed(wsrv::as_bytes(request));
    ASSERT_EQ(outcome.step, HandshakeStep::Accepted);
    ASSERT_TRUE(outcome.deflate.has_value());
    EXPECT_TRUE(outcome.deflate->params.server_no_context_takeover);  // the policy default
    EXPECT_EQ(outcome.deflate->params.server_window_bits, 12);
    EXPECT_NE(outcome.response.find("Sec-WebSocket-Extensions: " + outcome.deflate->response),
              std::string::npos);
}

TEST(HandshakeDriverDeflate, NoAgreementIsCarriedWithoutAnOfferOrWithoutAPolicy) {
    {  // a policy, but the client offers nothing
        wsrv::HandshakeConfig config = handshake_config();
        config.permessage_deflate = wsrv::DeflatePolicy{};
        const HandshakeNegotiator negotiator{config};
        HandshakeDriver driver{negotiator, wsrv::HttpLimits{}};
        EXPECT_FALSE(driver.feed(wsrv::as_bytes(kRequest)).deflate.has_value());
    }
    {  // an offer, but the server has no policy
        const HandshakeNegotiator negotiator{handshake_config()};
        HandshakeDriver driver{negotiator, wsrv::HttpLimits{}};
        std::string request{kRequest};
        request.insert(request.size() - 2, "Sec-WebSocket-Extensions: permessage-deflate\r\n");
        const wsrv::HandshakeOutcome outcome = driver.feed(wsrv::as_bytes(request));
        ASSERT_EQ(outcome.step, HandshakeStep::Accepted);
        EXPECT_FALSE(outcome.deflate.has_value());
    }
}

TEST_F(HandshakeDriverTest, ARejectedOutcomeCarriesNoAcceptKey) {
    std::string request{kRequest};
    request.replace(request.find("Version: 13"), 11, "Version: 12");
    const wsrv::HandshakeOutcome outcome = feed(request);
    ASSERT_EQ(outcome.step, HandshakeStep::Rejected);
    EXPECT_TRUE(outcome.accept_key.empty());
}

TEST_F(HandshakeDriverTest, TheRequestMayArriveOneByteAtATime) {  // EC-H17, FR-F1 spirit
    wsrv::HandshakeOutcome outcome;
    for (std::size_t i = 0; i < kRequest.size(); ++i) {
        outcome = feed(kRequest.substr(i, 1));
        if (i + 1 < kRequest.size()) {
            ASSERT_EQ(outcome.step, HandshakeStep::NeedMore) << "decided too early at byte " << i;
            EXPECT_TRUE(outcome.response.empty());
        }
    }
    ASSERT_EQ(outcome.step, HandshakeStep::Accepted);
    EXPECT_EQ(outcome.response, kAccepted);
}

TEST_F(HandshakeDriverTest, FrameBytesRightBehindTheHeadAreNotConsumed) {  // EC-H19, FR-H8
    const std::string stream = std::string{kRequest} + "\x81\x85";
    const wsrv::HandshakeOutcome outcome = feed(stream);
    ASSERT_EQ(outcome.step, HandshakeStep::Accepted);
    EXPECT_EQ(outcome.consumed, kRequest.size());
    EXPECT_EQ(stream.size() - outcome.consumed, 2U);  // the caller owns these bytes now
}

TEST_F(HandshakeDriverTest, ARefusedUpgradeCarriesItsStatusAndClosesTheConnection) {
    std::string request{kRequest};
    request.replace(request.find("Origin: https://app.example.com"),
                    std::string_view{"Origin: https://app.example.com"}.size(),
                    "Origin: https://evil.example");
    const wsrv::HandshakeOutcome outcome = feed(request);
    ASSERT_EQ(outcome.step, HandshakeStep::Rejected);
    EXPECT_EQ(outcome.response.rfind("HTTP/1.1 403 ", 0), 0U);
    EXPECT_NE(outcome.response.find("Connection: close\r\n"), std::string::npos);
}

TEST_F(HandshakeDriverTest, MalformedHttpIsRejectedWithTheParsersStatus) {       // EC-H15, FR-H9
    const wsrv::HandshakeOutcome outcome = feed("GET / HTTP/1.1\nHost: x\n\n");  // bare LF
    ASSERT_EQ(outcome.step, HandshakeStep::Rejected);
    EXPECT_EQ(outcome.response.rfind("HTTP/1.1 400 ", 0), 0U);
}

TEST_F(HandshakeDriverTest, AnOversizeHeadIsRejectedWith431) {  // FR-H7, T2
    wsrv::HttpLimits limits;
    limits.max_total_bytes = 64;
    HandshakeDriver small{negotiator, limits};
    const std::string flood = "GET / HTTP/1.1\r\n" + std::string(200, 'a');
    const wsrv::HandshakeOutcome outcome = small.feed(wsrv::as_bytes(flood));
    ASSERT_EQ(outcome.step, HandshakeStep::Rejected);
    EXPECT_EQ(outcome.response.rfind("HTTP/1.1 431 ", 0), 0U);
    EXPECT_EQ(outcome.consumed, flood.size());  // a refusal discards the whole chunk
}

TEST_F(HandshakeDriverTest, AfterTheDecisionFurtherInputIsInert) {
    ASSERT_EQ(feed(kRequest).step, HandshakeStep::Accepted);
    const wsrv::HandshakeOutcome again = feed(kRequest);
    EXPECT_EQ(again.step, HandshakeStep::Finished);
    EXPECT_EQ(again.consumed, 0U);
    EXPECT_TRUE(again.response.empty());

    HandshakeDriver rejected{negotiator, wsrv::HttpLimits{}};
    ASSERT_EQ(rejected.feed(wsrv::as_bytes(std::string_view{"\x16\x03\x01 garbage\r\n\r\n"})).step,
              HandshakeStep::Rejected);
    EXPECT_EQ(rejected.feed(wsrv::as_bytes(kRequest)).step, HandshakeStep::Finished);
}

TEST_F(HandshakeDriverTest, EmptyInputNeedsMore) {
    EXPECT_EQ(feed("").step, HandshakeStep::NeedMore);
}

}  // namespace
