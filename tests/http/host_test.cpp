// SPDX-License-Identifier: Apache-2.0
#include "http/host.hpp"

#include <gtest/gtest.h>

#include <string>
#include <vector>

namespace {

using wsrv::is_valid_host_header;

TEST(HostHeader, AcceptsRegNamesIpv4AndIpv6LiteralsWithOptionalPorts) {
    for (const char* host : {"example.com",
                             "example.com:8080",
                             "localhost",
                             "127.0.0.1:80",
                             "[::1]",
                             "[::1]:8080",
                             "[2001:db8::1]:443",
                             "a-b.c_d",
                             "xn--bcher-kva.example",
                             "%41.example",
                             "h:0",
                             "h:65535",
                             "[::ffff:192.0.2.1]"}) {
        EXPECT_TRUE(is_valid_host_header(host)) << host;
    }
}

TEST(HostHeader, RejectsEverythingElse) {
    const std::vector<std::string> bad{
        "",                   // empty
        " ",                  // blank
        "a b",                // space
        "a b, c",             // the shape a lenient server once accepted
        "exa mple.com",       // space inside
        "example.com:",       // empty port
        ":80",                // no host
        "example.com:99999",  // port out of range
        "example.com:65536",
        "example.com:80:80",   // two ports
        "example.com:ab",      // non-numeric port
        "example.com:080000",  // too many digits
        "[::1",                // unterminated literal
        "[::1]x",              // junk after the literal
        "[::1]:",              // empty port after a literal
        "[]",                  // empty literal
        "[zz]",                // not an address
        "[1:2]",               // too few groups to be IPv6
        "a@b.com",             // userinfo is not allowed in Host
        "a/b",                 // slash
        "a\\b",                // backslash
        "ex<ample>.com",       // angle brackets
        "caf\xC3\xA9.com",     // raw non-ASCII
        "%zz.example",         // bad percent-escape
        "%4",                  // truncated percent-escape
        "a,b",  // comma list is not one host (',' is a sub-delim but the header must name ONE host)
        std::string{"a\0b", 3},
        "a\tb",
    };
    for (const std::string& host : bad) {
        EXPECT_FALSE(is_valid_host_header(host)) << host;
    }
}

}  // namespace
