// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/token_format.hpp"

#include "base/bytes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "ipc/message.hpp"

// ADR-0038 section 3: over TCP the IPC token is the only authentication, and a length floor does
// not make a token strong ("MyCompanyWebsocketToken2026!!!!!" is long and guessable). So the daemon
// takes only the form `wsrvd --generate-token` writes. These tests pin that form and what it
// refuses. The expected strings were computed independently (Python: bytes.hex() and zlib.crc32),
// not by the code under test.

namespace {

using wsrv::wsrvd::crc32;
using wsrv::wsrvd::format_generated_token;
using wsrv::wsrvd::is_generated_token;
using wsrv::wsrvd::kGeneratedTokenRandomBytes;
using wsrv::wsrvd::kGeneratedTokenSize;

constexpr std::string_view kAllZero =
    "wsrv1_0000000000000000000000000000000000000000000000000000000000000000_e3805f5e";
constexpr std::string_view kCounting =
    "wsrv1_000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f_27994e12";

std::string token_of(std::uint8_t first, bool counting) {
    std::array<std::byte, kGeneratedTokenRandomBytes> random{};
    for (std::size_t i = 0; i < random.size(); ++i) {
        random[i] = static_cast<std::byte>(counting ? i : first);
    }
    return format_generated_token(wsrv::ByteView{random.data(), random.size()});
}

TEST(TokenFormat, Crc32MatchesTheStandardCheckValue) {
    constexpr std::string_view check = "123456789";
    EXPECT_EQ(crc32(wsrv::as_bytes(check)), 0xCBF43926U);
}

TEST(TokenFormat, TheGeneratedFormIsAPrefixHexOfThirtyTwoBytesAndAChecksum) {
    EXPECT_EQ(token_of(0, false), kAllZero);
    EXPECT_EQ(token_of(0, true), kCounting);
    EXPECT_EQ(kCounting.size(), kGeneratedTokenSize);
    EXPECT_EQ(kGeneratedTokenSize, 79U);
}

TEST(TokenFormat, WhatTheGeneratorWritesPassesTheCheck) {
    EXPECT_TRUE(is_generated_token(kAllZero));
    EXPECT_TRUE(is_generated_token(kCounting));
    EXPECT_TRUE(is_generated_token(token_of(0xAB, false)));
}

// The reason the form exists: a token somebody chose is refused, however long it is.
TEST(TokenFormat, ALongButChosenTokenIsRefused) {
    for (const std::string_view weak :
         {"MyCompanyWebsocketToken2026!!!!!",
          "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
          "changemechangemechangemechangeme",
          "correct horse battery staple password",
          "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef",
          ""}) {
        EXPECT_FALSE(is_generated_token(weak)) << weak;
    }
}

// A typo, a truncated copy and an edited digit all fail: the checksum is there for the operator who
// copies a token by hand. Every single-character change of a valid token, at every position.
TEST(TokenFormat, AnySingleCharacterChangeBreaksTheToken) {
    const std::string good{kCounting};
    for (std::size_t i = 0; i < good.size(); ++i) {
        std::string changed = good;
        changed[i] = changed[i] == 'e' ? 'f' : 'e';
        EXPECT_FALSE(is_generated_token(changed)) << "position " << i;
    }
}

TEST(TokenFormat, TruncatedOrExtendedOrDecoratedTokensAreRefused) {
    const std::string good{kCounting};
    EXPECT_FALSE(is_generated_token(good.substr(0, good.size() - 1)));
    EXPECT_FALSE(is_generated_token(good + "0"));
    EXPECT_FALSE(is_generated_token(good + "\n"));  // the file reader drops ONE newline before this
    EXPECT_FALSE(is_generated_token(" " + good));
    EXPECT_FALSE(is_generated_token(std::string{"wsrv2_"} + good.substr(6)));  // another version
    EXPECT_FALSE(is_generated_token(std::string{"WSRV1_"} + good.substr(6)));
    std::string with_nul = good;
    with_nul[10] = '\0';
    EXPECT_FALSE(is_generated_token(with_nul));
}

// Uppercase hex is another spelling of the same bytes; accepting it would give one token two
// valid spellings. The generator writes lowercase and that is the only form.
TEST(TokenFormat, UppercaseHexIsNotTheGeneratedForm) {
    std::string upper{kCounting};
    for (std::size_t i = 6; i < 6 + 64; ++i) {
        if (upper[i] >= 'a' && upper[i] <= 'f') {
            upper[i] = static_cast<char>(upper[i] - 'a' + 'A');
        }
    }
    ASSERT_NE(upper, kCounting);
    EXPECT_FALSE(is_generated_token(upper));
}

TEST(TokenFormat, ATokenFitsTheIpcTokenLimit) {
    EXPECT_LE(kGeneratedTokenSize, wsrv::ipc::kMaxTokenSize);  // HELLO cannot carry more
}

}  // namespace
