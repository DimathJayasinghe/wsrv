// SPDX-License-Identifier: Apache-2.0
// Host-name matching for SNI (ADR-0019 section 7, RFC 6066 section 3): pure functions, no TLS.
#include "tls/sni.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using wsrv::sni::matches;
using wsrv::sni::normalise_host;
using wsrv::sni::normalise_pattern;

TEST(SniPattern, ExactNamesAndOneLeadingWildcardAreAccepted) {
    EXPECT_EQ(normalise_pattern("www.example.com"), "www.example.com");
    EXPECT_EQ(normalise_pattern("localhost"), "localhost");  // a single label is a real name
    EXPECT_EQ(normalise_pattern("*.example.com"), "*.example.com");
    EXPECT_EQ(normalise_pattern("a_b.example-1.com"), "a_b.example-1.com");
}

TEST(SniPattern, NamesAreLowerCasedBecauseDnsIsCaseInsensitive) {
    EXPECT_EQ(normalise_pattern("WWW.Example.COM"), "www.example.com");
    EXPECT_EQ(normalise_pattern("*.EXAMPLE.com"), "*.example.com");
}

TEST(SniPattern, MalformedPatternsAreRefused) {
    for (const char* bad : {"",
                            ".",
                            ".example.com",  // empty first label
                            "example.com.",  // a trailing dot is not an SNI name (RFC 6066)
                            "a..b.com",      // empty label
                            "*",             // would match everything: that is the default's job
                            "*.",
                            "*.com",  // a wildcard over a whole top-level domain
                            "**.example.com",
                            "*.*.example.com",
                            "foo*.example.com",
                            "www.*.example.com",
                            "ex ample.com",
                            "exa\tmple.com",
                            "exämple.com",  // IDNs must be given in their ASCII (xn--) form
                            "a b.example.com"}) {
        EXPECT_FALSE(normalise_pattern(bad).has_value()) << '"' << bad << '"';
    }
}

TEST(SniPattern, LengthLimitsFollowDns) {
    const std::string label63(63, 'a');
    const std::string label64(64, 'a');
    EXPECT_TRUE(normalise_pattern(label63 + ".example.com").has_value());
    EXPECT_FALSE(normalise_pattern(label64 + ".example.com").has_value());
    // 253 characters is the longest DNS name in text form.
    const std::string longest =
        label63 + "." + label63 + "." + label63 + "." + std::string(61, 'b');
    ASSERT_EQ(longest.size(), 253U);
    EXPECT_TRUE(normalise_pattern(longest).has_value());
    EXPECT_FALSE(normalise_pattern(longest + "c").has_value());
}

TEST(SniHost, TheNameFromTheWireIsCheckedBeforeAnythingIsComparedWithIt) {
    EXPECT_EQ(normalise_host("Www.Example.COM"), "www.example.com");
    EXPECT_FALSE(normalise_host("").has_value());
    EXPECT_FALSE(normalise_host("example.com.").has_value());  // RFC 6066: no trailing dot
    EXPECT_FALSE(normalise_host("a..com").has_value());
    EXPECT_FALSE(
        normalise_host("*.example.com").has_value());  // a client cannot ask for a wildcard
    EXPECT_FALSE(normalise_host("exa mple.com").has_value());
    EXPECT_FALSE(normalise_host(std::string(254, 'a')).has_value());
    EXPECT_FALSE(normalise_host(std::string("a\0b.com", 7)).has_value());
}

TEST(SniMatch, AnExactPatternMatchesOnlyThatName) {
    EXPECT_TRUE(matches("www.example.com", "www.example.com"));
    EXPECT_FALSE(matches("www.example.com", "example.com"));
    EXPECT_FALSE(matches("www.example.com", "xwww.example.com"));
    EXPECT_FALSE(matches("www.example.com", "www.example.com.evil.org"));
}

TEST(SniMatch, AWildcardCoversExactlyOneLabel) {  // RFC 6125 section 6.4.3 style
    EXPECT_TRUE(matches("*.example.com", "a.example.com"));
    EXPECT_TRUE(matches("*.example.com", "a-b_1.example.com"));
    EXPECT_FALSE(matches("*.example.com", "example.com"));      // zero labels
    EXPECT_FALSE(matches("*.example.com", "a.b.example.com"));  // two labels
    EXPECT_FALSE(matches("*.example.com", ".example.com"));     // an empty label
    EXPECT_FALSE(matches("*.example.com", "aexample.com"));     // the dot is part of the pattern
    EXPECT_FALSE(matches("*.example.com", "a.example.com.evil"));
    EXPECT_FALSE(matches("*.example.com", "a.xexample.com"));
}

}  // namespace
