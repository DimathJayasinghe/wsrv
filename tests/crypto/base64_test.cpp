// SPDX-License-Identifier: Apache-2.0
#include "crypto/base64.hpp"

#include "base/bytes.hpp"
#include "support/fakes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <string>
#include <string_view>
#include <vector>

namespace {

using wsrv::ErrorCode;

std::string encode_text(std::string_view text) {
    return wsrv::base64_encode(wsrv::as_bytes(text));
}

std::string decode_to_text(std::string_view encoded) {
    const auto decoded = wsrv::base64_decode(encoded);
    EXPECT_TRUE(decoded.has_value()) << encoded;
    return decoded ? std::string{wsrv::as_chars(*decoded)} : std::string{};
}

// ---- RFC 4648 section 10 test vectors, both directions
// ---------------------------------------------

TEST(Base64, Rfc4648VectorsEncode) {
    EXPECT_EQ(encode_text(""), "");
    EXPECT_EQ(encode_text("f"), "Zg==");
    EXPECT_EQ(encode_text("fo"), "Zm8=");
    EXPECT_EQ(encode_text("foo"), "Zm9v");
    EXPECT_EQ(encode_text("foob"), "Zm9vYg==");
    EXPECT_EQ(encode_text("fooba"), "Zm9vYmE=");
    EXPECT_EQ(encode_text("foobar"), "Zm9vYmFy");
}

TEST(Base64, Rfc4648VectorsDecode) {
    EXPECT_EQ(decode_to_text(""), "");
    EXPECT_EQ(decode_to_text("Zg=="), "f");
    EXPECT_EQ(decode_to_text("Zm8="), "fo");
    EXPECT_EQ(decode_to_text("Zm9v"), "foo");
    EXPECT_EQ(decode_to_text("Zm9vYg=="), "foob");
    EXPECT_EQ(decode_to_text("Zm9vYmE="), "fooba");
    EXPECT_EQ(decode_to_text("Zm9vYmFy"), "foobar");
}

TEST(Base64, UsesPlusAndSlashForSextets62And63) {
    const std::array<std::byte, 2> two{std::byte{0xFB}, std::byte{0xFF}};
    EXPECT_EQ(wsrv::base64_encode(two), "+/8=");
    const std::array<std::byte, 3> three{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}};
    EXPECT_EQ(wsrv::base64_encode(three), "////");
    EXPECT_EQ(wsrv::base64_decode("+/8=").value(),
              (std::vector<std::byte>{std::byte{0xFB}, std::byte{0xFF}}));
}

// ---- the WebSocket key (EC-H07)
// --------------------------------------------------------------------

TEST(Base64, RfcSampleKeyDecodesToSixteenBytes) {
    const auto key = wsrv::base64_decode("dGhlIHNhbXBsZSBub25jZQ==");
    ASSERT_TRUE(key.has_value());
    EXPECT_EQ(key->size(), 16U);
    EXPECT_EQ(wsrv::as_chars(*key), "the sample nonce");
}

// ---- strictness: everything below must be rejected with InvalidBase64
// -----------------------------

TEST(Base64, RejectsMalformedInput) {
    const std::vector<std::string> bad{
        "Zg",                     // missing padding
        "Zg=",                    // padding too short for the length
        "Z===",                   // too much padding
        "====",                   // nothing but padding
        "=Zg=",                   // padding at the start
        "Zg=a",                   // data after padding
        "Zg==Zg==",               // padding in the middle (two messages glued together)
        "Zm9v=",                  // length not a multiple of 4
        "Zm9v\n",                 // 5 characters: rejected by the length rule
        "Zm9\n",                  // 4 characters: newline where a data character belongs
        "Zm 9",                   // embedded space inside a group
        " Zm9",                   // leading space
        "Zm9v Zm9v",              // space between two valid groups
        "Zm9v\r\nZm9v",           // line-wrapped (MIME style) input
        std::string{"Z\0==", 4},  // NUL byte
        "Zm-_",                   // URL-safe alphabet is a different encoding
        "Zm9\xC3",                // non-ASCII byte
        "Zh==",                   // non-canonical: low 4 bits of the 2nd sextet must be 0
        "Zk==",  // bit 2 of the 4 unused bits set (a mask of 0x03 would wrongly accept it)
        "ZI==",  // bit 3 set
        "QR==",  // another set-bit pattern
        "QUJ=",  // 3rd sextet J has its low 2 bits set
        "Zm9=",  // non-canonical: low 2 bits of the 3rd sextet must be 0
        "Zm+=",  // non-canonical, uses '+'
    };
    for (const std::string& text : bad) {
        const auto result = wsrv::base64_decode(text);
        ASSERT_FALSE(result.has_value()) << "accepted: " << text;
        EXPECT_EQ(result.error().code, ErrorCode::InvalidBase64);
    }
}

TEST(Base64, AcceptsCanonicalFormsNextToTheRejectedOnes) {
    EXPECT_EQ(decode_to_text("ZA=="), "d");
    EXPECT_EQ(decode_to_text("QQ=="), "A");
    EXPECT_EQ(decode_to_text("QUI="), "AB");
    EXPECT_EQ(decode_to_text("Zm9vZm9v"), "foofoo");
}

TEST(Base64, EveryNonAlphabetByteIsRejected) {
    constexpr std::string_view kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (int value = 0; value < 256; ++value) {
        const char c = static_cast<char>(value);
        const std::string text{'Z', 'm', '9', c};
        const bool in_alphabet = kAlphabet.find(c) != std::string_view::npos;
        EXPECT_EQ(wsrv::base64_decode(text).has_value(), in_alphabet) << "byte " << value;
    }
}

// ---- round trip
// -------------------------------------------------------------------------------------

TEST(Base64, RoundTripsEveryLengthFromZeroTo300) {
    wsrv::test::FakeRandom random;
    for (std::size_t length = 0; length <= 300; ++length) {
        std::vector<std::byte> data(length);
        ASSERT_TRUE(random.fill(data).has_value());
        const std::string encoded = wsrv::base64_encode(data);
        ASSERT_EQ(encoded.size() % 4, 0U);
        const auto decoded = wsrv::base64_decode(encoded);
        ASSERT_TRUE(decoded.has_value()) << "length " << length;
        ASSERT_EQ(*decoded, data) << "length " << length;
    }
}

}  // namespace
