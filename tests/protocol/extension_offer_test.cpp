// SPDX-License-Identifier: Apache-2.0
#include "protocol/extension_offer.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

using wsrv::parse_extension_offers;
using Params = std::vector<std::pair<std::string, std::string>>;

TEST(ExtensionOffer, ParsesAnExtensionWithoutParameters) {
    const auto offers = parse_extension_offers("permessage-deflate");
    ASSERT_TRUE(offers.has_value());
    ASSERT_EQ(offers->size(), 1U);
    EXPECT_EQ((*offers)[0].name, "permessage-deflate");
    EXPECT_TRUE((*offers)[0].params.empty());
}

TEST(ExtensionOffer, ParsesParametersWithAndWithoutValues) {
    const auto offers = parse_extension_offers(
        "permessage-deflate; client_max_window_bits=10; server_no_context_takeover");
    ASSERT_TRUE(offers.has_value());
    ASSERT_EQ(offers->size(), 1U);
    EXPECT_EQ((*offers)[0].params,
              (Params{{"client_max_window_bits", "10"}, {"server_no_context_takeover", ""}}));
}

TEST(ExtensionOffer, ParsesAParameterWithoutValueOnItsOwn) {
    const auto offers = parse_extension_offers("permessage-deflate; client_max_window_bits");
    ASSERT_TRUE(offers.has_value());
    EXPECT_EQ((*offers)[0].params, (Params{{"client_max_window_bits", ""}}));
}

TEST(ExtensionOffer, QuotedStringValuesAreUnquotedAndUnescaped) {
    const auto offers = parse_extension_offers(R"(ext; a="15"; b="x\y"; c="a-b")");
    ASSERT_TRUE(offers.has_value());
    EXPECT_EQ((*offers)[0].params, (Params{{"a", "15"}, {"b", "xy"}, {"c", "a-b"}}));
}

TEST(ExtensionOffer, ARuleOfRfc6455QuotedValuesMustBeTokensAfterUnescaping) {
    for (const char* text :
         {R"(a; b="x y")", R"(a; b="1,2")", R"(a; b="x;y")", R"(a; b="")", R"(a; b="x\"y")"}) {
        EXPECT_FALSE(parse_extension_offers(text).has_value()) << text;
    }
}

TEST(ExtensionOffer, CapsBoundTheWorkAnAttackerCanBuyWithOneHeader) {
    std::string many_params = "ext";
    for (std::size_t i = 0; i < wsrv::kMaxExtensionParams; ++i) {
        many_params += "; p" + std::to_string(i);
    }
    EXPECT_TRUE(parse_extension_offers(many_params).has_value());
    EXPECT_FALSE(parse_extension_offers(many_params + "; one-too-many").has_value());

    std::string many_offers;
    for (std::size_t i = 0; i < wsrv::kMaxExtensionOffers; ++i) {
        many_offers += (i == 0 ? "e" : ", e") + std::to_string(i);
    }
    EXPECT_TRUE(parse_extension_offers(many_offers).has_value());
    EXPECT_FALSE(parse_extension_offers(many_offers + ", e-too-many").has_value());
}

TEST(ExtensionOffer, MultipleOffersAreSeparatedByCommasAndKeepTheirOrder) {
    const auto offers =
        parse_extension_offers("permessage-deflate; client_max_window_bits, x-foo; a=b ,, bar");
    ASSERT_TRUE(offers.has_value());
    ASSERT_EQ(offers->size(), 3U);
    EXPECT_EQ((*offers)[0].name, "permessage-deflate");
    EXPECT_EQ((*offers)[1].name, "x-foo");
    EXPECT_EQ((*offers)[1].params, (Params{{"a", "b"}}));
    EXPECT_EQ((*offers)[2].name, "bar");
}

TEST(ExtensionOffer, EmptyOrBlankHeaderMeansNoOffers) {
    for (const char* text : {"", "   ", ",", " , ,"}) {
        const auto offers = parse_extension_offers(text);
        ASSERT_TRUE(offers.has_value()) << text;
        EXPECT_TRUE(offers->empty()) << text;
    }
}

TEST(ExtensionOffer, MalformedOffersAreRejected) {
    const std::vector<std::string> bad{
        "permessage-deflate;",  // dangling semicolon
        ";foo",                 // missing extension name
        "a;;b",                 // empty parameter
        "a; b=",                // '=' without value
        "a; =c",                // value without name
        "a; b=\"unterminated",  // open quote
        "a; b=c d",             // two tokens where one is expected
        "a b",                  // space inside the name
        "a; b = c",             // no whitespace allowed around '='
        "a; b=c; b=d",          // duplicate parameter
        "a; b=\"x\" y",         // junk after a quoted value
        "a; b=\"bad\\",         // backslash at the end of input
        "a(b)",                 // separator in the name
        "a; b=c,",              // fine list, but see next entry for strictness
    };
    for (std::size_t i = 0; i + 1 < bad.size(); ++i) {
        EXPECT_FALSE(parse_extension_offers(bad[i]).has_value()) << bad[i];
    }
    EXPECT_TRUE(
        parse_extension_offers(bad.back()).has_value());  // a trailing comma is an empty element
}

TEST(ExtensionOffer, ControlCharactersAreRejected) {
    EXPECT_FALSE(parse_extension_offers(std::string{"a; b=\"x\0y\"", 10}).has_value());
    EXPECT_FALSE(parse_extension_offers("a; b=\"x\ty\x01\"").has_value());
}

}  // namespace
