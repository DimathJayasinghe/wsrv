// SPDX-License-Identifier: Apache-2.0
#include "crypto/sha1.hpp"

#include "base/bytes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <string>
#include <vector>

#ifdef WSRV_TEST_HAS_OPENSSL
#include <openssl/sha.h>
#endif

namespace {

using wsrv::ByteView;
using wsrv::Sha1;

std::string hex(const Sha1::Digest& digest) {
    constexpr std::string_view kDigits = "0123456789abcdef";
    std::string out;
    for (const std::byte b : digest) {
        const auto v = std::to_integer<unsigned>(b);
        out += kDigits[v >> 4U];
        out += kDigits[v & 0xFU];
    }
    return out;
}

std::string sha1_hex(std::string_view text) {
    return hex(Sha1::hash(wsrv::as_bytes(text)));
}

// ---- RFC 3174 and well-known vectors
// -------------------------------------------------------------

TEST(Sha1, EmptyMessage) {
    EXPECT_EQ(sha1_hex(""), "da39a3ee5e6b4b0d3255bfef95601890afd80709");
}

TEST(Sha1, Abc) {
    EXPECT_EQ(sha1_hex("abc"), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(Sha1, Rfc3174TwoBlockMessage) {
    EXPECT_EQ(sha1_hex("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
              "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
}

TEST(Sha1, OneMillionAsInSingleUpdateAndInChunks) {
    const std::string million(1'000'000, 'a');
    EXPECT_EQ(sha1_hex(million), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");

    Sha1 hasher;
    for (int i = 0; i < 1000; ++i) {
        hasher.update(wsrv::as_bytes(std::string_view{million}.substr(0, 1000)));
    }
    EXPECT_EQ(hex(hasher.finish()), "34aa973cd4c4daa4f61eeb2bdbad27316534016f");
}

// ---- padding boundaries: 55 fits one block, 56 needs a second, 64 is a full block
// ----------------- Expected digests generated with Python hashlib.

TEST(Sha1, PaddingBoundaryLengths) {
    struct Case {
        std::size_t length;
        const char* digest;
    };
    const std::array<Case, 7> cases{{
        {55, "cef734ba81a024479e09eb5a75b6ddae62e6abf1"},
        {56, "901305367c259952f4e7af8323f480d59f81335b"},
        {63, "0ddc4e0cccd9a12850deb5abb0853a4425559fec"},
        {64, "bb2fa3ee7afb9f54c6dfb5d021f14b1ffe40c163"},
        {65, "78c741ddc482e4cdf8c474a0876347a0905b6233"},
        {119, "4300320394f7ee239bcdce7d3b8bcee173a0cd5c"},
        {120, "ceb2821639c4b6dcb10bce0e522ca2e608ce056d"},
    }};
    for (const Case& c : cases) {
        EXPECT_EQ(sha1_hex(std::string(c.length, 'x')), c.digest) << "length " << c.length;
    }
}

// ---- the WebSocket handshake (RFC 6455 section 1.3; EC-H09)
// ---------------------------------------

TEST(Sha1, WebSocketAcceptInputFromTheRfc) {
    constexpr std::string_view kKeyAndGuid =
        "dGhlIHNhbXBsZSBub25jZQ==258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    EXPECT_EQ(sha1_hex(kKeyAndGuid), "b37a4f2cc0624f1690f64606cf385945b2bec4ea");
}

// ---- streaming: any chunking gives the same digest
// -------------------------------------------------

TEST(Sha1, EverySplitPointGivesTheSameDigest) {
    constexpr std::string_view kMessage =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
    const std::string expected = sha1_hex(kMessage);
    for (std::size_t split = 0; split <= kMessage.size(); ++split) {
        Sha1 hasher;
        hasher.update(wsrv::as_bytes(kMessage.substr(0, split)));
        hasher.update(wsrv::as_bytes(kMessage.substr(split)));
        ASSERT_EQ(hex(hasher.finish()), expected) << "split at " << split;
    }
}

TEST(Sha1, ByteAtATimeGivesTheSameDigest) {
    const std::string message(200, 'q');
    Sha1 hasher;
    for (const char c : message) {
        hasher.update(wsrv::as_bytes(std::string_view{&c, 1}));
    }
    EXPECT_EQ(hex(hasher.finish()), sha1_hex(message));
}

TEST(Sha1, RandomChunkSizesMatchOneShot) {
    std::mt19937 rng{2024};
    std::string message(5000, '\0');
    for (char& c : message) {
        c = static_cast<char>(rng() & 0xFFU);
    }
    Sha1 hasher;
    for (std::size_t pos = 0; pos < message.size();) {
        const std::size_t take = std::min<std::size_t>(rng() % 150, message.size() - pos);
        hasher.update(wsrv::as_bytes(std::string_view{message}.substr(pos, take)));
        pos += take;
    }
    EXPECT_EQ(hex(hasher.finish()), sha1_hex(message));
}

TEST(Sha1, FinishResetsSoTheObjectIsReusable) {
    Sha1 hasher;
    hasher.update(wsrv::as_bytes("garbage that must not leak into the next message"));
    (void)hasher.finish();
    hasher.update(wsrv::as_bytes("abc"));
    EXPECT_EQ(hex(hasher.finish()), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST(Sha1, EmptyUpdatesAreHarmless) {
    Sha1 hasher;
    hasher.update({});
    hasher.update(wsrv::as_bytes("abc"));
    hasher.update({});
    EXPECT_EQ(hex(hasher.finish()), "a9993e364706816aba3e25717850c26c9cd0d89d");
}

// ---- differential test against OpenSSL (only when it is installed)
// ---------------------------------

#ifdef WSRV_TEST_HAS_OPENSSL
TEST(Sha1, AgreesWithOpenSslOnRandomInputs) {
    std::mt19937 rng{777};
    for (int round = 0; round < 2000; ++round) {
        std::vector<std::byte> data(rng() % 300);
        for (std::byte& b : data) {
            b = static_cast<std::byte>(rng() & 0xFFU);
        }
        std::vector<unsigned char> raw(data.size());
        std::transform(data.begin(), data.end(), raw.begin(), [](std::byte b) {
            return std::to_integer<unsigned char>(b);
        });
        std::array<unsigned char, SHA_DIGEST_LENGTH> expected{};
        SHA1(raw.data(), raw.size(), expected.data());
        const Sha1::Digest actual = Sha1::hash(data);
        for (std::size_t i = 0; i < actual.size(); ++i) {
            ASSERT_EQ(std::to_integer<unsigned>(actual[i]), expected[i]) << "round " << round;
        }
    }
}
#endif

}  // namespace
