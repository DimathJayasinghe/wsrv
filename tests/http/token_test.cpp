// SPDX-License-Identifier: Apache-2.0
#include "http/token.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using wsrv::contains_token;
using wsrv::iequals;
using wsrv::is_tchar;
using wsrv::is_token;
using wsrv::split_list;
using wsrv::trim_ows;

TEST(Token, TcharMatchesRfc9110ForEveryByteValue) {
    // RFC 9110 section 5.6.2: "!#$%&'*+-.^_`|~" plus DIGIT and ALPHA. Written out independently of
    // the code.
    const std::string allowed_punct = "!#$%&'*+-.^_`|~";
    for (int value = 0; value < 256; ++value) {
        const char c = static_cast<char>(value);
        const bool alnum = (value >= '0' && value <= '9') || (value >= 'A' && value <= 'Z') ||
                           (value >= 'a' && value <= 'z');
        const bool expected = alnum || allowed_punct.find(c) != std::string::npos;
        EXPECT_EQ(is_tchar(c), expected) << "byte " << value;
    }
}

TEST(Token, TokenNeedsAtLeastOneCharacterAndOnlyTchars) {
    EXPECT_TRUE(is_token("websocket"));
    EXPECT_TRUE(is_token("Sec-WebSocket-Key"));
    EXPECT_TRUE(is_token("x-custom_header.1"));
    EXPECT_FALSE(is_token(""));
    EXPECT_FALSE(is_token("two words"));
    EXPECT_FALSE(is_token("a:b"));
    EXPECT_FALSE(is_token("a,b"));
    EXPECT_FALSE(is_token("a;b"));
    EXPECT_FALSE(is_token("quoted\"x"));
    EXPECT_FALSE(is_token(std::string_view{"nul\0byte", 8}));
    EXPECT_FALSE(is_token("caf\xC3\xA9"));
}

TEST(Token, CaseInsensitiveComparisonIsAsciiOnly) {
    EXPECT_TRUE(iequals("WebSocket", "websocket"));
    EXPECT_TRUE(iequals("UPGRADE", "Upgrade"));
    EXPECT_FALSE(iequals("upgrade", "upgrad"));
    EXPECT_FALSE(iequals("upgrade", "upgradf"));
    EXPECT_TRUE(iequals("", ""));
    // Bytes >= 0x80 are never folded, whatever the process locale is.
    EXPECT_FALSE(iequals("\xC3\x89", "\xC3\xA9"));
    // The Turkish-locale trap: 'I' must fold to 'i' and nothing else.
    EXPECT_TRUE(iequals("I", "i"));
    EXPECT_FALSE(iequals("I", "\xC4\xB1"));  // dotless i in UTF-8
    // '@' (0x40) and '`' (0x60) differ by 0x20 but are not letters: they must not compare equal.
    EXPECT_FALSE(iequals("@", "`"));
    EXPECT_FALSE(iequals("[", "{"));
}

TEST(Token, TrimOwsRemovesOnlySpacesAndTabsAtTheEnds) {
    EXPECT_EQ(trim_ows("  x \t"), "x");
    EXPECT_EQ(trim_ows("\t\ta b\t"), "a b");
    EXPECT_EQ(trim_ows(""), "");
    EXPECT_EQ(trim_ows(" \t "), "");
    EXPECT_EQ(trim_ows("x"), "x");
    EXPECT_EQ(trim_ows("\nx\n"), "\nx\n");  // not OWS
}

TEST(Token, SplitListFollowsRfc9110ListRules) {
    EXPECT_EQ(split_list("a, b ,,c"), (std::vector<std::string_view>{"a", "b", "c"}));
    EXPECT_EQ(split_list(""), std::vector<std::string_view>{});
    EXPECT_EQ(split_list(" , ,"), std::vector<std::string_view>{});
    EXPECT_EQ(split_list("single"), (std::vector<std::string_view>{"single"}));
    EXPECT_EQ(split_list("keep-alive, Upgrade"),
              (std::vector<std::string_view>{"keep-alive", "Upgrade"}));
}

TEST(Token, ContainsTokenMatchesWholeTokensCaseInsensitively) {
    EXPECT_TRUE(contains_token("keep-alive, Upgrade", "upgrade"));
    EXPECT_TRUE(contains_token("UPGRADE", "upgrade"));
    EXPECT_FALSE(contains_token("keep-alive", "upgrade"));
    EXPECT_FALSE(contains_token("upgraded", "upgrade"));  // not a substring match
    EXPECT_FALSE(contains_token("x-upgrade", "upgrade"));
    EXPECT_FALSE(contains_token("", "upgrade"));
}

}  // namespace
