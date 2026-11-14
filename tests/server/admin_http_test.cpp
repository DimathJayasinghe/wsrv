// SPDX-License-Identifier: Apache-2.0
// The admin page logic (ADR-0023 section 3) without sockets: which request gets which answer.
// The request head is peer data: every shape of garbage must end in a short, well-formed answer.
#include "server/admin_http.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using wsrv::AdminSources;

AdminSources sources(bool ready = true) {
    return AdminSources{[] { return std::string{"wsrv_connections_current 3\n"}; },
                        [ready] { return ready; }};
}

std::string answer(std::string_view request, bool ready = true) {
    return wsrv::admin_response(request, sources(ready));
}

bool starts_with(const std::string& text, std::string_view prefix) {
    return text.rfind(prefix, 0) == 0;
}

std::string body_of(const std::string& response) {
    const std::size_t split = response.find("\r\n\r\n");
    return split == std::string::npos ? std::string{} : response.substr(split + 4);
}

TEST(AdminHttp, MetricsServesThePrometheusTextWithItsLength) {
    const std::string response = answer("GET /metrics HTTP/1.1\r\nHost: x\r\n\r\n");
    EXPECT_TRUE(starts_with(response, "HTTP/1.1 200 OK\r\n")) << response;
    EXPECT_NE(response.find("Content-Type: text/plain; version=0.0.4; charset=utf-8\r\n"),
              std::string::npos)
        << response;
    EXPECT_NE(response.find("Content-Length: 27\r\n"), std::string::npos) << response;
    EXPECT_NE(response.find("Connection: close\r\n"), std::string::npos);
    EXPECT_EQ(body_of(response), "wsrv_connections_current 3\n");
}

TEST(AdminHttp, HealthzIsAlwaysOkWhileTheLoopAnswers) {
    const std::string response = answer("GET /healthz HTTP/1.1\r\n\r\n", /*ready=*/false);
    EXPECT_TRUE(starts_with(response, "HTTP/1.1 200 OK\r\n")) << response;
    EXPECT_EQ(body_of(response), "ok\n");
}

TEST(AdminHttp, ReadyzFollowsTheReadinessOfTheServer) {
    EXPECT_TRUE(starts_with(answer("GET /readyz HTTP/1.1\r\n\r\n", true), "HTTP/1.1 200 OK\r\n"));
    const std::string not_ready = answer("GET /readyz HTTP/1.1\r\n\r\n", false);
    EXPECT_TRUE(starts_with(not_ready, "HTTP/1.1 503 Service Unavailable\r\n")) << not_ready;
    EXPECT_EQ(body_of(not_ready), "not ready\n");
}

TEST(AdminHttp, HttpOneZeroIsAcceptedAndAnyOtherVersionIsNot) {
    EXPECT_TRUE(starts_with(answer("GET /healthz HTTP/1.0\r\n\r\n"), "HTTP/1.1 200 OK\r\n"));
    EXPECT_TRUE(starts_with(answer("GET /healthz HTTP/2.0\r\n\r\n"), "HTTP/1.1 400 "));
    EXPECT_TRUE(starts_with(answer("GET /healthz HTTP/1.11\r\n\r\n"), "HTTP/1.1 400 "));
    EXPECT_TRUE(starts_with(answer("GET /healthz\r\n\r\n"), "HTTP/1.1 400 "));
}

TEST(AdminHttp, OnlyGetIsAllowedAndTheAnswerSaysSo) {
    for (const char* method : {"POST", "PUT", "DELETE", "HEAD", "OPTIONS", "get", "CONNECT"}) {
        const std::string response = answer(std::string{method} + " /metrics HTTP/1.1\r\n\r\n");
        EXPECT_TRUE(starts_with(response, "HTTP/1.1 405 Method Not Allowed\r\n")) << method;
        EXPECT_NE(response.find("Allow: GET\r\n"), std::string::npos) << method;
    }
}

TEST(AdminHttp, AnyOtherTargetIsNotFound) {
    for (const char* target : {"/",
                               "/metrics/",
                               "/metrics?x=1",
                               "/METRICS",
                               "/healthz%20",
                               "/../metrics",
                               "*",
                               "http://h/metrics",
                               "/metricsx"}) {
        const std::string response = answer(std::string{"GET "} + target + " HTTP/1.1\r\n\r\n");
        EXPECT_TRUE(starts_with(response, "HTTP/1.1 404 Not Found\r\n")) << target;
    }
}

TEST(AdminHttp, MalformedRequestLinesAreBadRequests) {
    const std::string with_nul{"GET /me\0trics HTTP/1.1\r\n\r\n", 26};
    for (const std::string_view request : {std::string_view{"\r\n\r\n"},
                                           std::string_view{"GET\r\n\r\n"},
                                           std::string_view{"GET /metrics\r\n\r\n"},
                                           std::string_view{"GET  /metrics HTTP/1.1\r\n\r\n"},
                                           std::string_view{"GET /metrics HTTP/1.1 extra\r\n\r\n"},
                                           std::string_view{"GET /metrics HTTP/1.1\n\n"},
                                           std::string_view{"\x16\x03\x01\x02\x00\x01\x00\r\n\r\n"},
                                           std::string_view{with_nul}}) {
        const std::string response = answer(request);
        EXPECT_TRUE(starts_with(response, "HTTP/1.1 400 Bad Request\r\n")) << response;
        EXPECT_EQ(body_of(response).back(), '\n');
    }
}

TEST(AdminHttp, EveryAnswerIsCompleteAndSmall) {
    for (const std::string_view request : {std::string_view{"GET /metrics HTTP/1.1\r\n\r\n"},
                                           std::string_view{"nonsense\r\n\r\n"},
                                           std::string_view{"POST /x HTTP/1.1\r\n\r\n"}}) {
        const std::string response = answer(request);
        const std::size_t split = response.find("\r\n\r\n");
        ASSERT_NE(split, std::string::npos);
        const std::string head = response.substr(0, split);
        const std::size_t length_at = head.find("Content-Length: ");
        ASSERT_NE(length_at, std::string::npos) << head;
        EXPECT_EQ(std::stoul(head.substr(length_at + 16)), response.size() - split - 4);
        EXPECT_LT(response.size(), std::size_t{20} * 1024);
    }
}

TEST(AdminHttp, TheHeadIsCompleteOnlyAfterTheBlankLine) {
    EXPECT_FALSE(wsrv::admin_head_complete(""));
    EXPECT_FALSE(wsrv::admin_head_complete("GET /metrics HTTP/1.1\r\nHost: x\r\n"));
    EXPECT_FALSE(wsrv::admin_head_complete("GET /metrics HTTP/1.1\r\n\r"));
    EXPECT_TRUE(wsrv::admin_head_complete("GET /metrics HTTP/1.1\r\n\r\n"));
    EXPECT_TRUE(wsrv::admin_head_complete("GET /metrics HTTP/1.1\r\n\r\nGARBAGE-AFTER"));
}

}  // namespace
