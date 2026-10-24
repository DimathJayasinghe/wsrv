// SPDX-License-Identifier: Apache-2.0
#include "protocol/close_code.hpp"

#include "base/bytes.hpp"
#include "protocol/close_payload.hpp"
#include "protocol/utf8.hpp"

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string>
#include <vector>

namespace {

using wsrv::CloseCode;

TEST(CloseCode, NamedCodesHaveTheirRfcValues) {
    EXPECT_EQ(static_cast<unsigned>(CloseCode::Normal), 1000U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::GoingAway), 1001U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::ProtocolError), 1002U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::UnsupportedData), 1003U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::NoStatusReceived), 1005U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::AbnormalClosure), 1006U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::InvalidPayload), 1007U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::PolicyViolation), 1008U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::MessageTooBig), 1009U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::MandatoryExtension), 1010U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::InternalError), 1011U);
    EXPECT_EQ(static_cast<unsigned>(CloseCode::TlsHandshake), 1015U);
}

TEST(CloseCode, ValidOnTheWireMatchesAnIndependentDefinitionForEveryValue) {  // EC-C07, EC-C08
    std::set<unsigned> valid{
        1000, 1001, 1002, 1003, 1007, 1008, 1009, 1010, 1011, 1012, 1013, 1014};
    for (unsigned code = 3000; code <= 4999; ++code) {
        valid.insert(code);
    }
    for (unsigned code = 0; code <= 0xFFFF; ++code) {
        EXPECT_EQ(wsrv::is_valid_on_wire(static_cast<std::uint16_t>(code)), valid.contains(code))
            << code;
    }
}

TEST(CloseCode, LocalOnlyCodesAreNeverValidOnTheWire) {
    for (const auto code :
         {CloseCode::NoStatusReceived, CloseCode::AbnormalClosure, CloseCode::TlsHandshake}) {
        EXPECT_FALSE(wsrv::is_valid_on_wire(static_cast<std::uint16_t>(code)));
    }
}

// ---- parse_close_payload
// ---------------------------------------------------------------------------------

std::vector<std::byte> payload(unsigned code, std::string_view reason = {}) {
    std::vector<std::byte> out(2 + reason.size());
    wsrv::store_be16(out, static_cast<std::uint16_t>(code));
    const auto text = wsrv::as_bytes(reason);
    std::copy(text.begin(), text.end(), out.begin() + 2);
    return out;
}

TEST(ClosePayload, EmptyPayloadMeansNoStatus) {  // EC-C05
    const auto parsed = wsrv::parse_close_payload({});
    ASSERT_TRUE(parsed.has_value());
    EXPECT_FALSE(parsed->has_value());
}

TEST(ClosePayload, OneByteIsAProtocolError) {  // EC-C06
    const std::array<std::byte, 1> one{std::byte{0x03}};
    const auto parsed = wsrv::parse_close_payload(one);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, CloseCode::ProtocolError);
}

TEST(ClosePayload, CodeAndReasonAreExtracted) {
    const auto bytes = payload(1001, "server shutting down");
    const auto parsed = wsrv::parse_close_payload(bytes);
    ASSERT_TRUE(parsed.has_value());
    ASSERT_TRUE(parsed->has_value());
    EXPECT_EQ((*parsed)->code, 1001U);
    EXPECT_EQ((*parsed)->reason, "server shutting down");
}

TEST(ClosePayload, CodeAloneIsFine) {
    const auto bytes = payload(1000);
    const auto parsed = wsrv::parse_close_payload(bytes);
    ASSERT_TRUE(parsed.has_value() && parsed->has_value());
    EXPECT_TRUE((*parsed)->reason.empty());
}

TEST(ClosePayload, InvalidCodesAreAProtocolError) {  // EC-C07
    for (const unsigned code :
         {0U, 999U, 1004U, 1005U, 1006U, 1015U, 1016U, 2000U, 2999U, 5000U, 65535U}) {
        const auto bytes = payload(code);
        const auto parsed = wsrv::parse_close_payload(bytes);
        ASSERT_FALSE(parsed.has_value()) << code;
        EXPECT_EQ(parsed.error().code, CloseCode::ProtocolError) << code;
    }
}

TEST(ClosePayload, ValidCodesAreAccepted) {  // EC-C08
    for (const unsigned code :
         {1000U, 1001U, 1002U, 1003U, 1007U, 1008U, 1009U, 1010U, 1011U, 3000U, 4999U}) {
        const auto bytes = payload(code);
        EXPECT_TRUE(wsrv::parse_close_payload(bytes).has_value()) << code;
    }
}

TEST(ClosePayload, ReasonLongerThan123BytesIsAProtocolError) {  // EC-C09
    const auto ok = payload(1000, std::string(123, 'a'));
    EXPECT_TRUE(wsrv::parse_close_payload(ok).has_value());
    const auto too_long = payload(1000, std::string(124, 'a'));
    const auto parsed = wsrv::parse_close_payload(too_long);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, CloseCode::ProtocolError);
}

TEST(ClosePayload, ReasonMustBeValidUtf8) {  // EC-U08
    const auto bytes = payload(1000, "bad \xC3\x28 reason");
    const auto parsed = wsrv::parse_close_payload(bytes);
    ASSERT_FALSE(parsed.has_value());
    EXPECT_EQ(parsed.error().code, CloseCode::InvalidPayload);
    EXPECT_TRUE(wsrv::parse_close_payload(payload(1000, "caf\xC3\xA9 \xE2\x82\xAC")).has_value());
}

// ---- encode_close_payload
// ---------------------------------------------------------------------------------

TEST(ClosePayload, EncodingRoundTrips) {
    std::array<std::byte, wsrv::kMaxControlPayload> buffer{};
    const std::size_t n = wsrv::encode_close_payload(buffer, CloseCode::GoingAway, "bye");
    EXPECT_EQ(n, 5U);
    const auto parsed = wsrv::parse_close_payload(wsrv::ByteView{buffer}.first(n));
    ASSERT_TRUE(parsed.has_value() && parsed->has_value());
    EXPECT_EQ((*parsed)->code, 1001U);
    EXPECT_EQ((*parsed)->reason, "bye");
}

TEST(ClosePayload, LongReasonsAreTruncatedWithoutSplittingACodePoint) {
    std::array<std::byte, wsrv::kMaxControlPayload> buffer{};
    std::string three_byte_chars;
    for (int i = 0; i < 60; ++i) {
        three_byte_chars += "\xE2\x82\xAC";  // euro sign, 3 bytes each
    }
    const std::vector<std::string> texts{
        std::string(500, 'a'),                       // plain ASCII
        std::string(122, 'a') + "\xC3\xA9",          // 124 bytes: a 2-byte char straddles the limit
        three_byte_chars,                            // 180 bytes of 3-byte characters
        std::string(121, 'a') + "\xF0\x9F\x98\x80",  // a 4-byte char straddles the limit
    };
    for (const std::string& text : texts) {
        const std::size_t n = wsrv::encode_close_payload(buffer, CloseCode::Normal, text);
        ASSERT_LE(n, wsrv::kMaxControlPayload);
        const auto parsed = wsrv::parse_close_payload(wsrv::ByteView{buffer}.first(n));
        ASSERT_TRUE(parsed.has_value() && parsed->has_value()) << text.size();
        EXPECT_LE((*parsed)->reason.size(), 123U);
        EXPECT_TRUE(wsrv::is_valid_utf8(wsrv::as_bytes((*parsed)->reason))) << text.size();
        EXPECT_EQ(text.compare(0, (*parsed)->reason.size(), (*parsed)->reason),
                  0);  // a true prefix
    }
}

TEST(ClosePayloadDeathTest, LocalOnlyCodesMustNeverBeEncoded) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    std::array<std::byte, wsrv::kMaxControlPayload> buffer{};
    EXPECT_DEATH(
        { (void)wsrv::encode_close_payload(buffer, CloseCode::AbnormalClosure, ""); },
        "WSRV_ASSERT failed");
}

}  // namespace
