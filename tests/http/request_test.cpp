// SPDX-License-Identifier: Apache-2.0
#include "http/request.hpp"

#include <gtest/gtest.h>

#include <string_view>
#include <vector>

namespace {

wsrv::HttpRequest sample() {
    wsrv::HttpRequest request;
    request.method = "GET";
    request.target = "/chat";
    request.headers = {{"Host", "example.com"},
                       {"Sec-WebSocket-Protocol", "chat"},
                       {"sec-websocket-protocol", "superchat, other"},
                       {"X-Empty", ""}};
    return request;
}

TEST(HttpRequest, ValuesAreFoundCaseInsensitivelyInArrivalOrder) {
    const wsrv::HttpRequest request = sample();
    EXPECT_EQ(request.values("SEC-WEBSOCKET-PROTOCOL"),
              (std::vector<std::string_view>{"chat", "superchat, other"}));
    EXPECT_EQ(request.values("host"), (std::vector<std::string_view>{"example.com"}));
}

TEST(HttpRequest, MissingHeaderYieldsNoValues) {
    EXPECT_TRUE(sample().values("Origin").empty());
    EXPECT_EQ(sample().count("Origin"), 0U);
}

TEST(HttpRequest, CountReflectsDuplicatesAndEmptyValues) {
    const wsrv::HttpRequest request = sample();
    EXPECT_EQ(request.count("Sec-WebSocket-Protocol"), 2U);
    EXPECT_EQ(request.count("x-empty"), 1U);
    EXPECT_EQ(request.values("X-Empty"), (std::vector<std::string_view>{""}));
}

TEST(HttpRequest, DefaultsAreHttp11) {
    const wsrv::HttpRequest request;
    EXPECT_EQ(request.version_minor, 1);
    EXPECT_TRUE(request.headers.empty());
}

}  // namespace
