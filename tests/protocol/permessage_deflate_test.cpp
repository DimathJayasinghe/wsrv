// SPDX-License-Identifier: Apache-2.0
// permessage-deflate negotiation (RFC 7692 sections 5 to 7, ADR-0020 section 2, EC-D01).
#include "protocol/permessage_deflate.hpp"

#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

namespace {

using wsrv::DeflatePolicy;
using wsrv::ExtensionOffer;
using wsrv::negotiate_permessage_deflate;

ExtensionOffer offer(std::string name,
                     std::vector<std::pair<std::string, std::string>> params = {}) {
    return ExtensionOffer{std::move(name), std::move(params)};
}

ExtensionOffer pmd(std::vector<std::pair<std::string, std::string>> params = {}) {
    return offer("permessage-deflate", std::move(params));
}

/// The answer to a single offer under `policy`, or "<declined>".
std::string answer(const ExtensionOffer& one, const DeflatePolicy& policy = {}) {
    const std::vector<ExtensionOffer> offers{one};
    const auto agreement = negotiate_permessage_deflate(offers, policy);
    return agreement ? agreement->response : "<declined>";
}

DeflatePolicy relaxed() {  // no server-side restrictions beyond the protocol maximum
    DeflatePolicy policy;
    policy.server_no_context_takeover = false;
    policy.server_max_window_bits = 15;
    policy.client_max_window_bits = 15;
    return policy;
}

TEST(PermessageDeflate, ABareOfferIsAcceptedUnderThePolicyDefaults) {
    EXPECT_EQ(answer(pmd()), "permessage-deflate; server_no_context_takeover; server_max_window_bits=12");
}

TEST(PermessageDeflate, ARelaxedPolicyAnswersWithNothingExtra) {
    EXPECT_EQ(answer(pmd(), relaxed()), "permessage-deflate");
}

TEST(PermessageDeflate, TheAgreedParametersAreReturnedForTheCodec) {
    const std::vector<ExtensionOffer> offers{
        pmd({{"client_no_context_takeover", ""}, {"server_max_window_bits", "10"},
             {"client_max_window_bits", "11"}})};
    const auto agreement = negotiate_permessage_deflate(offers, DeflatePolicy{});
    ASSERT_TRUE(agreement.has_value());
    EXPECT_TRUE(agreement->params.server_no_context_takeover);  // from the policy
    EXPECT_TRUE(agreement->params.client_no_context_takeover);  // asked for and echoed
    EXPECT_EQ(agreement->params.server_window_bits, 10);        // min(offered 10, policy 12)
    EXPECT_EQ(agreement->params.client_window_bits, 11);
}

TEST(PermessageDeflate, ServerNoContextTakeoverIsIncludedWheneverTheClientAsksEvenIfPolicyAllowsTakeover) {
    EXPECT_EQ(answer(pmd({{"server_no_context_takeover", ""}}), relaxed()),
              "permessage-deflate; server_no_context_takeover");
}

TEST(PermessageDeflate, ClientNoContextTakeoverIsEchoedWhenOffered) {
    EXPECT_EQ(answer(pmd({{"client_no_context_takeover", ""}}), relaxed()),
              "permessage-deflate; client_no_context_takeover");
}

TEST(PermessageDeflate, TheServerWindowIsTheSmallerOfTheOfferAndThePolicy) {
    DeflatePolicy policy = relaxed();
    policy.server_max_window_bits = 12;
    EXPECT_EQ(answer(pmd({{"server_max_window_bits", "10"}}), policy),
              "permessage-deflate; server_max_window_bits=10");
    EXPECT_EQ(answer(pmd({{"server_max_window_bits", "14"}}), policy),
              "permessage-deflate; server_max_window_bits=12");
    EXPECT_EQ(answer(pmd({{"server_max_window_bits", "9"}}), policy),
              "permessage-deflate; server_max_window_bits=9");
}

TEST(PermessageDeflate, AServerWindowOfEightIsDeclinedBecauseZlibCannotDoIt) {
    // zlib raises 8 to 9 internally; a 9-bit stream may break an 8-bit decoder, so say no.
    EXPECT_EQ(answer(pmd({{"server_max_window_bits", "8"}}), relaxed()), "<declined>");
}

TEST(PermessageDeflate, BadServerWindowValuesDeclineTheOffer) {  // RFC 7692 section 7.1.2.1
    for (const char* bad : {"", "7", "16", "99", "abc", "09", "+10", "-10", "1 0", "10.0", "0"}) {
        EXPECT_EQ(answer(pmd({{"server_max_window_bits", bad}}), relaxed()), "<declined>")
            << '"' << bad << '"';
    }
}

TEST(PermessageDeflate, TheClientWindowIsOnlyAnsweredWhenTheClientOfferedIt) {  // section 7.1.2.2
    EXPECT_EQ(answer(pmd(), relaxed()), "permessage-deflate");  // not offered: not in the answer
    EXPECT_EQ(answer(pmd({{"client_max_window_bits", ""}}), relaxed()),
              "permessage-deflate; client_max_window_bits=15");
}

TEST(PermessageDeflate, TheClientWindowIsTheSmallerOfTheOfferAndThePolicy) {
    DeflatePolicy policy = relaxed();
    policy.client_max_window_bits = 11;
    EXPECT_EQ(answer(pmd({{"client_max_window_bits", "14"}}), policy),
              "permessage-deflate; client_max_window_bits=11");
    EXPECT_EQ(answer(pmd({{"client_max_window_bits", "9"}}), policy),
              "permessage-deflate; client_max_window_bits=9");
    // The server's own decoder always uses the full 15-bit window, so a client window of 8 works.
    EXPECT_EQ(answer(pmd({{"client_max_window_bits", "8"}}), policy),
              "permessage-deflate; client_max_window_bits=8");
}

TEST(PermessageDeflate, BadClientWindowValuesDeclineTheOffer) {
    for (const char* bad : {"7", "16", "abc", "08", "+9", "-9", "9 "}) {
        EXPECT_EQ(answer(pmd({{"client_max_window_bits", bad}}), relaxed()), "<declined>")
            << '"' << bad << '"';
    }
}

TEST(PermessageDeflate, ValuesOnParametersThatTakeNoneDeclineTheOffer) {
    EXPECT_EQ(answer(pmd({{"server_no_context_takeover", "1"}}), relaxed()), "<declined>");
    EXPECT_EQ(answer(pmd({{"client_no_context_takeover", "true"}}), relaxed()), "<declined>");
}

TEST(PermessageDeflate, AnUnknownParameterDeclinesTheOfferNotTheConnection) {  // section 5.2
    EXPECT_EQ(answer(pmd({{"unknown_parameter", ""}}), relaxed()), "<declined>");
}

TEST(PermessageDeflate, OfferOrderIsKeptAndTheFirstAcceptableOneWins) {
    const std::vector<ExtensionOffer> offers{
        pmd({{"server_max_window_bits", "99"}}),   // unacceptable
        pmd({{"server_max_window_bits", "10"}}),   // this one
        pmd({{"server_max_window_bits", "11"}})};
    const auto agreement = negotiate_permessage_deflate(offers, relaxed());
    ASSERT_TRUE(agreement.has_value());
    EXPECT_EQ(agreement->response, "permessage-deflate; server_max_window_bits=10");
}

TEST(PermessageDeflate, OtherExtensionsAreIgnoredAndNeverEchoed) {
    const std::vector<ExtensionOffer> offers{offer("x-webkit-deflate-frame"), offer("foo"), pmd()};
    const auto agreement = negotiate_permessage_deflate(offers, relaxed());
    ASSERT_TRUE(agreement.has_value());
    EXPECT_EQ(agreement->response, "permessage-deflate");
    EXPECT_FALSE(negotiate_permessage_deflate(std::vector<ExtensionOffer>{offer("foo")}, relaxed())
                     .has_value());
    EXPECT_FALSE(negotiate_permessage_deflate(std::vector<ExtensionOffer>{}, relaxed()).has_value());
}

TEST(PermessageDeflate, AnExtensionNameIsMatchedExactly) {
    EXPECT_EQ(answer(offer("permessage-deflate2")), "<declined>");
    EXPECT_EQ(answer(offer("Permessage-Deflate")), "<declined>");  // tokens are case-sensitive here
}

TEST(PermessageDeflate, ABrowserStyleOfferGetsAUsableAnswer) {
    // What Chrome sends: client_max_window_bits without a value.
    const std::string chrome = answer(pmd({{"client_max_window_bits", ""}}));
    EXPECT_EQ(chrome,
              "permessage-deflate; server_no_context_takeover; server_max_window_bits=12; "
              "client_max_window_bits=15");
}

TEST(DeflatePolicyCheck, WindowsOutsideTheSupportedRangeAreInvalid) {
    DeflatePolicy ok;
    EXPECT_TRUE(wsrv::valid(ok));
    for (const int bad : {-1, 0, 8, 16, 100}) {
        DeflatePolicy server = ok;
        server.server_max_window_bits = bad;
        EXPECT_FALSE(wsrv::valid(server)) << bad;
        DeflatePolicy client = ok;
        client.client_max_window_bits = bad;
        EXPECT_FALSE(wsrv::valid(client)) << bad;
    }
}

}  // namespace
