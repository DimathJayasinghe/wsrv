// SPDX-License-Identifier: Apache-2.0
#include "net/endpoint.hpp"

#include <gtest/gtest.h>

#include <string>

namespace {

using wsrv::Endpoint;
using wsrv::ErrorCode;

TEST(Endpoint, ParsesIpv4Literals) {
    const auto e = wsrv::parse_endpoint("127.0.0.1", 8080);
    ASSERT_TRUE(e.has_value());
    EXPECT_FALSE(e->v6);
    EXPECT_EQ(e->port, 8080);
    EXPECT_EQ(e->address[0], 127);
    EXPECT_EQ(e->address[3], 1);
    EXPECT_EQ(wsrv::format_endpoint(*e), "127.0.0.1:8080");
}

TEST(Endpoint, ParsesIpv6LiteralsIncludingLoopbackWildcardAndMappedIpv4) {  // EC-N18
    const auto loopback = wsrv::parse_endpoint("::1", 443);
    ASSERT_TRUE(loopback.has_value());
    EXPECT_TRUE(loopback->v6);
    EXPECT_EQ(loopback->address[15], 1);
    EXPECT_EQ(wsrv::format_endpoint(*loopback), "[::1]:443");
    const auto any = wsrv::parse_endpoint("::", 0);
    ASSERT_TRUE(any.has_value());
    EXPECT_EQ(wsrv::format_endpoint(*any), "[::]:0");
    const auto mapped = wsrv::parse_endpoint("::ffff:10.1.2.3", 1);
    ASSERT_TRUE(mapped.has_value());
    EXPECT_TRUE(mapped->v6);  // stays a v6 address; per-IP limits normalise it later (EC-N18)
    EXPECT_EQ(mapped->address[10], 0xFF);
    EXPECT_EQ(mapped->address[12], 10);
}

TEST(Endpoint, RejectsEverythingThatIsNotALiteralAddress) {
    for (const char* bad : {"",
                            "localhost",
                            "example.com",
                            "256.0.0.1",
                            "1.2.3",
                            "1.2.3.4.5",
                            " 1.2.3.4",
                            "1.2.3.4 ",
                            "01.2.3.4x",
                            "::g",
                            "[::1]",
                            "1.2.3.4:80",
                            "0x7f.0.0.1"}) {
        const auto e = wsrv::parse_endpoint(bad, 80);
        EXPECT_FALSE(e.has_value()) << bad;
        if (!e.has_value()) {
            EXPECT_EQ(e.error().code, ErrorCode::InvalidArgument) << bad;
        }
    }
    EXPECT_FALSE(
        wsrv::parse_endpoint(std::string(100, '1'), 80).has_value());  // longer than any address
}

TEST(Endpoint, EqualEndpointsCompareEqual) {
    EXPECT_EQ(*wsrv::parse_endpoint("10.0.0.1", 1), *wsrv::parse_endpoint("10.0.0.1", 1));
    EXPECT_NE(*wsrv::parse_endpoint("10.0.0.1", 1), *wsrv::parse_endpoint("10.0.0.1", 2));
    EXPECT_NE(*wsrv::parse_endpoint("10.0.0.1", 1), *wsrv::parse_endpoint("10.0.0.2", 1));
}

}  // namespace
