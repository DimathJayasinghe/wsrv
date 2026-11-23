// SPDX-License-Identifier: Apache-2.0
// The client address behind a reverse proxy (ADR-0029, EC-N21): which header entry is the client,
// when a header is believed at all, and that anything odd falls back to "no answer" (the caller
// then uses the proxy's own address, so the limits only get stricter). Pure functions, no sockets.
#include "server/client_ip.hpp"

#include "net/endpoint.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using wsrv::client_address;
using wsrv::Endpoint;
using wsrv::ForwardedStyle;
using wsrv::IpNet;
using wsrv::is_trusted_proxy;
using wsrv::parse_ip_net;

Endpoint at(std::string_view ip) {
    auto parsed = wsrv::parse_endpoint(ip, 0);
    EXPECT_TRUE(parsed.has_value()) << ip;
    return *parsed;
}

std::vector<IpNet> nets(std::initializer_list<std::string_view> texts) {
    std::vector<IpNet> out;
    for (const std::string_view text : texts) {
        auto net = parse_ip_net(text);
        EXPECT_TRUE(net.has_value()) << text;
        out.push_back(*net);
    }
    return out;
}

/// The answer of a list header (X-Forwarded-For) as text, or "none".
std::string list_answer(const std::vector<IpNet>& trusted, std::vector<std::string_view> lines) {
    const auto client = client_address(trusted, ForwardedStyle::List, lines);
    return client ? wsrv::format_endpoint(*client) : "none";
}

TEST(ClientIpNet, ParsesAddressesAndRanges) {
    EXPECT_TRUE(parse_ip_net("10.0.0.0/8").has_value());
    EXPECT_TRUE(parse_ip_net("192.168.1.5").has_value());  // a single host: /32
    EXPECT_TRUE(parse_ip_net("::1").has_value());
    EXPECT_TRUE(parse_ip_net("fd00::/8").has_value());
    EXPECT_TRUE(
        parse_ip_net("0.0.0.0/0").has_value());  // everyone: allowed, but the operator wrote it
}

TEST(ClientIpNet, RefusesWhatIsNotAnAddressOrRange) {
    for (const std::string_view bad : {"",
                                       "/",
                                       "10.0.0.0/",
                                       "10.0.0.0/33",
                                       "10.0.0.0/-1",
                                       "10.0.0.0/08x",
                                       "::1/129",
                                       "localhost",
                                       "10.0.0.0/8/8",
                                       " 10.0.0.1",
                                       "10.0.0.1 ",
                                       "10.0.0.256",
                                       "[::1]",
                                       "1.2.3.4:80",
                                       "fe80::1%eth0"}) {
        EXPECT_FALSE(parse_ip_net(bad).has_value()) << "'" << bad << "'";
    }
}

TEST(ClientIpNet, MatchesByPrefix) {
    const auto trusted = nets({"10.0.0.0/8", "192.168.1.5", "fd00::/8"});
    EXPECT_TRUE(is_trusted_proxy(trusted, at("10.1.2.3")));
    EXPECT_TRUE(is_trusted_proxy(trusted, at("10.255.255.255")));
    EXPECT_FALSE(is_trusted_proxy(trusted, at("11.0.0.1")));
    EXPECT_TRUE(is_trusted_proxy(trusted, at("192.168.1.5")));
    EXPECT_FALSE(is_trusted_proxy(trusted, at("192.168.1.6")));
    EXPECT_TRUE(is_trusted_proxy(trusted, at("fd12::1")));
    EXPECT_FALSE(is_trusted_proxy(trusted, at("fe80::1")));
    EXPECT_FALSE(is_trusted_proxy({}, at("10.1.2.3"))) << "an empty list trusts nobody";
}

TEST(ClientIpNet, AnIpv4MappedAddressIsTheSameHost) {
    const auto trusted = nets({"10.0.0.0/8"});
    EXPECT_TRUE(is_trusted_proxy(trusted, at("::ffff:10.1.2.3")))
        << "a dual-stack listener shows IPv4 peers like this";
    const auto mapped = nets({"::ffff:10.0.0.0/104"});
    EXPECT_TRUE(is_trusted_proxy(mapped, at("10.9.9.9")));
}

TEST(ClientIpList, TheRightmostUntrustedEntryIsTheClient) {
    const auto trusted = nets({"10.0.0.0/8"});
    EXPECT_EQ("203.0.113.7:0", list_answer(trusted, {"203.0.113.7"}));
    EXPECT_EQ("203.0.113.7:0", list_answer(trusted, {"203.0.113.7, 10.0.0.2"}))
        << "the second hop is ours";
    EXPECT_EQ("203.0.113.7:0", list_answer(trusted, {"203.0.113.7, 10.0.0.2, 10.0.0.3"}));
}

TEST(ClientIpList, WhatLiesLeftOfTheClientIsNeverUsed) {
    const auto trusted = nets({"10.0.0.0/8"});
    // A client may put anything in front of the entry our proxy appended.
    EXPECT_EQ("203.0.113.7:0", list_answer(trusted, {"1.1.1.1, 2.2.2.2, 203.0.113.7"}));
    EXPECT_EQ("203.0.113.7:0", list_answer(trusted, {"10.0.0.99, 203.0.113.7"}))
        << "a forged 'trusted' entry on the left";
}

TEST(ClientIpList, WhenEveryHopIsTrustedTheLeftmostIsTheClient) {
    const auto trusted = nets({"10.0.0.0/8"});
    EXPECT_EQ("10.0.0.5:0", list_answer(trusted, {"10.0.0.5, 10.0.0.2"}));
    EXPECT_EQ("10.0.0.5:0", list_answer(trusted, {"10.0.0.5"}));
}

TEST(ClientIpList, SeveralHeaderLinesAreOneListInOrder) {
    const auto trusted = nets({"10.0.0.0/8"});
    EXPECT_EQ("203.0.113.7:0", list_answer(trusted, {"203.0.113.7", "10.0.0.2"}));
    EXPECT_EQ("10.0.0.2:0", list_answer(trusted, {"10.0.0.2", "10.0.0.3"}));
}

TEST(ClientIpList, AcceptsPortsBracketsAndIpv6) {
    const auto trusted = nets({"10.0.0.0/8"});
    EXPECT_EQ("203.0.113.7:0", list_answer(trusted, {"203.0.113.7:51234"}));
    EXPECT_EQ("[2001:db8::1]:0", list_answer(trusted, {"2001:db8::1"}));
    EXPECT_EQ("[2001:db8::1]:0", list_answer(trusted, {"[2001:db8::1]"}));
    EXPECT_EQ("[2001:db8::1]:0", list_answer(trusted, {"[2001:db8::1]:443"}));
    EXPECT_EQ("203.0.113.7:0", list_answer(trusted, {"  203.0.113.7  ,\t10.0.0.2 "}))
        << "optional whitespace around entries";
}

TEST(ClientIpList, AnythingUnusableGivesNoAnswer) {
    const auto trusted = nets({"10.0.0.0/8"});
    for (const std::string_view bad : {"",
                                       " ",
                                       ",",
                                       "unknown",
                                       "203.0.113.7, unknown",
                                       "203.0.113.256",
                                       "203.0.113.7:",
                                       "203.0.113.7:abc",
                                       "203.0.113.7:99999999",
                                       "[::1",
                                       "[::1]x",
                                       "fe80::1%eth0",
                                       "203.0.113.7;evil",
                                       "_hidden",
                                       "203.0.113.7,,10.0.0.2"}) {
        EXPECT_EQ("none", list_answer(trusted, {bad})) << "'" << bad << "'";
    }
    EXPECT_EQ("none", list_answer(trusted, {})) << "no header at all";
}

TEST(ClientIpList, TheListIsBounded) {
    const auto trusted = nets({"10.0.0.0/8"});
    std::string many;
    for (int i = 0; i < 33; ++i) {
        many += (i == 0 ? "" : ", ") + std::string{"10.0.0."} + std::to_string(i + 1);
    }
    EXPECT_EQ("none", list_answer(trusted, {many})) << "33 entries are too many";
    std::string ok;
    for (int i = 0; i < 32; ++i) {
        ok += (i == 0 ? "" : ", ") + std::string{"10.0.0."} + std::to_string(i + 1);
    }
    EXPECT_EQ("10.0.0.1:0", list_answer(trusted, {ok})) << "32 are the limit";
    EXPECT_EQ("none", list_answer(trusted, {std::string(2000, ' ') + "203.0.113.7"}))
        << "a value over 1 KiB";
}

TEST(ClientIpSingle, TheOneValueIsTheClient) {
    const auto trusted = nets({"10.0.0.0/8"});
    const std::vector<std::string_view> one{"203.0.113.7"};
    const auto client = client_address(trusted, ForwardedStyle::Single, one);
    ASSERT_TRUE(client.has_value());
    EXPECT_EQ("203.0.113.7:0", wsrv::format_endpoint(*client));
}

TEST(ClientIpSingle, NotOneValueOrNotAnAddressGivesNoAnswer) {
    const auto trusted = nets({"10.0.0.0/8"});
    const std::vector<std::string_view> two{"203.0.113.7", "198.51.100.1"};
    const std::vector<std::string_view> none{};
    const std::vector<std::string_view> list_like{"203.0.113.7, 198.51.100.1"};
    const std::vector<std::string_view> junk{"junk"};
    EXPECT_FALSE(client_address(trusted, ForwardedStyle::Single, two).has_value());
    EXPECT_FALSE(client_address(trusted, ForwardedStyle::Single, none).has_value());
    EXPECT_FALSE(client_address(trusted, ForwardedStyle::Single, list_like).has_value());
    EXPECT_FALSE(client_address(trusted, ForwardedStyle::Single, junk).has_value());
}

TEST(ClientIpHeader, ListSemanticsAreForXForwardedForOnly) {
    EXPECT_EQ(ForwardedStyle::List, wsrv::forwarded_style("X-Forwarded-For"));
    EXPECT_EQ(ForwardedStyle::List, wsrv::forwarded_style("x-forwarded-for"));
    EXPECT_EQ(ForwardedStyle::Single, wsrv::forwarded_style("X-Real-IP"));
    EXPECT_EQ(ForwardedStyle::Single, wsrv::forwarded_style("CF-Connecting-IP"));
}

}  // namespace
