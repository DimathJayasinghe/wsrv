// SPDX-License-Identifier: Apache-2.0
// The admin page logic (ADR-0023 section 3) without sockets: which request gets which answer.
// The request head is peer data: every shape of garbage must end in a short, well-formed answer.
#include "server/admin_http.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>

namespace {

using wsrv::AdminSources;

AdminSources sources(bool ready = true) {  // no token: the pages as they were before ADR-0041
    AdminSources out;
    out.metrics_text = [] { return std::string{"wsrv_connections_current 3\n"}; };
    out.ready = [ready] { return ready; };
    return out;
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

// ADR-0041: with a token configured only /metrics asks for a credential. Everything the 401 must
// and must not do is pinned here, without sockets.

constexpr std::string_view kToken =
    "wsrv1_4f2a9c0b7e1d3a5c8b6f0e2d4a6c8e0b1d3f5a7c9e1b3d5f7a9c1e3b5d7f9a1c_deadbeef";

/// Counts how often the page is rendered and how often a credential was refused.
struct Counters {
    int rendered = 0;
    int refused = 0;
};

AdminSources guarded(Counters& counters, bool ready = true) {
    AdminSources out;
    out.metrics_text = [&counters] {
        ++counters.rendered;
        return std::string{"wsrv_connections_current 3\n"};
    };
    out.ready = [ready] { return ready; };
    out.token = std::string{kToken};
    out.unauthorized = [&counters] { ++counters.refused; };
    return out;
}

std::string metrics_with(std::string_view header_lines, Counters& counters) {
    const std::string request =
        "GET /metrics HTTP/1.1\r\nHost: wsrvd:9100\r\n" + std::string{header_lines} + "\r\n";
    return wsrv::admin_response(request, guarded(counters));
}

std::string bearer(std::string_view credential) {
    return "Authorization: Bearer " + std::string{credential} + "\r\n";
}

TEST(AdminHttpToken, TheRightCredentialGetsTheMetrics) {
    Counters counters;
    const std::string response = metrics_with(bearer(kToken), counters);
    EXPECT_TRUE(starts_with(response, "HTTP/1.1 200 OK\r\n")) << response;
    EXPECT_EQ(body_of(response), "wsrv_connections_current 3\n");
    EXPECT_EQ(counters.rendered, 1);
    EXPECT_EQ(counters.refused, 0);
}

TEST(AdminHttpToken, ARealPrometheusScrapeHeadPasses) {  // the headers a scraper really sends
    Counters counters;
    const std::string head =
        std::string{"Accept: application/openmetrics-text;version=1.0.0,text/plain;q=0.5\r\n"} +
        "Accept-Encoding: gzip\r\nUser-Agent: Prometheus/3.0.0\r\n" + bearer(kToken) +
        "X-Prometheus-Scrape-Timeout-Seconds: 10\r\n";
    EXPECT_TRUE(starts_with(metrics_with(head, counters), "HTTP/1.1 200 OK\r\n"));
}

TEST(AdminHttpToken, NoWrongRepeatedOrMalformedCredentialGetsOneAnswerAndNoMetrics) {
    Counters counters;
    const std::string reference = metrics_with("", counters);  // none
    EXPECT_TRUE(starts_with(reference, "HTTP/1.1 401 Unauthorized\r\n")) << reference;
    EXPECT_NE(reference.find("WWW-Authenticate: Bearer\r\n"), std::string::npos) << reference;
    EXPECT_NE(reference.find("Connection: close\r\n"), std::string::npos);
    EXPECT_EQ(body_of(reference), "credential required\n");

    const std::string wrong_length_prefix = bearer(kToken.substr(0, kToken.size() - 1));
    const std::string wrong_length_longer = bearer(std::string{kToken} + "0");
    const std::string same_length_other =
        bearer(std::string{kToken.substr(0, kToken.size() - 1)} + "0");
    for (const std::string& header :
         {bearer("wrong"),
          bearer(""),
          wrong_length_prefix,
          wrong_length_longer,
          same_length_other,
          std::string{"Authorization: Basic dXNlcjpwYXNz\r\n"},
          std::string{"Authorization: Bearer\r\n"},
          "Authorization: Bearer  " + std::string{kToken} + "\r\n",
          bearer(kToken) + bearer(kToken),   // two headers, even equal
          bearer("wrong") + bearer(kToken),  // a good one behind a bad one
          bearer(kToken) + bearer("wrong"),  // and the other way round
          "X-Authorization: Bearer " + std::string{kToken} + "\r\n"}) {
        EXPECT_EQ(metrics_with(header, counters), reference)
            << "every refusal is the same bytes: the peer learns nothing about what was wrong";
    }
    EXPECT_EQ(counters.rendered, 0) << "the page is never rendered for a peer that is not allowed";
}

// RFC 9110 section 5.5: optional whitespace around a field value is not part of the value, and the
// head parser strips it. So a trailing space or a tab after the credential is the same credential,
// while a space INSIDE it (below, and in bearer_test) is not.
TEST(AdminHttpToken, WhitespaceAroundTheHeaderValueIsNotPartOfTheCredential) {
    Counters counters;
    for (const std::string& header : {"Authorization: Bearer " + std::string{kToken} + " \r\n",
                                      "Authorization:   Bearer " + std::string{kToken} + "\r\n",
                                      "Authorization:\tBearer " + std::string{kToken} + "\t\r\n"}) {
        EXPECT_TRUE(starts_with(metrics_with(header, counters), "HTTP/1.1 200 OK\r\n")) << header;
    }
}

TEST(AdminHttpToken, TheSchemeAndTheHeaderNameAreCaseInsensitive) {
    Counters counters;
    EXPECT_TRUE(
        starts_with(metrics_with("authorization: BEARER " + std::string{kToken} + "\r\n", counters),
                    "HTTP/1.1 200 OK\r\n"));
}

// The constraint behind the whole design: a kubelet httpGet probe and `wsrvd --healthcheck` cannot
// send a credential, so a token on these pages would take every pod out of service.
TEST(AdminHttpToken, TheProbesNeedNoCredentialEvenWhenATokenIsSet) {
    Counters counters;
    const AdminSources sources = guarded(counters);
    const std::string healthz =
        wsrv::admin_response("GET /healthz HTTP/1.1\r\nHost: x\r\n\r\n", sources);
    EXPECT_TRUE(starts_with(healthz, "HTTP/1.1 200 OK\r\n")) << healthz;
    const std::string readyz =
        wsrv::admin_response("GET /readyz HTTP/1.1\r\nHost: x\r\n\r\n", sources);
    EXPECT_TRUE(starts_with(readyz, "HTTP/1.1 200 OK\r\n")) << readyz;
    Counters not_ready_counters;
    const std::string not_ready =
        wsrv::admin_response("GET /readyz HTTP/1.1\r\n\r\n", guarded(not_ready_counters, false));
    EXPECT_TRUE(starts_with(not_ready, "HTTP/1.1 503 Service Unavailable\r\n")) << not_ready;
    EXPECT_EQ(counters.refused, 0) << "a probe is not an unauthorised request";
}

TEST(AdminHttpToken, RoutingComesBeforeTheCredential) {
    Counters counters;
    const AdminSources sources = guarded(counters);
    EXPECT_TRUE(starts_with(wsrv::admin_response("GET /other HTTP/1.1\r\n\r\n", sources),
                            "HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(starts_with(wsrv::admin_response("POST /metrics HTTP/1.1\r\n\r\n", sources),
                            "HTTP/1.1 405 Method Not Allowed\r\n"));
    EXPECT_TRUE(starts_with(wsrv::admin_response("GET /metrics?x=1 HTTP/1.1\r\n\r\n", sources),
                            "HTTP/1.1 404 Not Found\r\n"));
    EXPECT_TRUE(starts_with(wsrv::admin_response("nonsense\r\n\r\n", sources), "HTTP/1.1 400 "));
    EXPECT_EQ(counters.refused, 0);
}

TEST(AdminHttpToken, AHeadTheStrictParserRefusesIsABadRequestNotAnAnswerThatLooksAuthorised) {
    Counters counters;
    for (const std::string& header :
         {bearer(kToken) + "X-A: a\x01"
                           "b\r\n",            // a control character in a value
          bearer(kToken) + " folded\r\n",      // obs-fold: a line that starts with a space
          bearer(kToken) + "Bad Name: v\r\n",  // a header name that is not a token
          bearer(kToken) + "NoColon\r\n"}) {
        const std::string response = metrics_with(header, counters);
        EXPECT_TRUE(starts_with(response, "HTTP/1.1 400 Bad Request\r\n")) << response;
    }
    EXPECT_EQ(counters.rendered, 0);
}

TEST(AdminHttpToken, TheRefusalIsReportedOncePerRefusedRequestAndNeverForAnythingElse) {
    Counters counters;
    static_cast<void>(metrics_with("", counters));
    static_cast<void>(metrics_with(bearer("wrong"), counters));
    EXPECT_EQ(counters.refused, 2);
    static_cast<void>(metrics_with(bearer(kToken), counters));
    EXPECT_EQ(counters.refused, 2) << "an allowed request is not reported";
}

TEST(AdminHttpToken, AMissingReportingHookIsAllowed) {  // tests and embedders may not want one
    Counters counters;
    AdminSources sources = guarded(counters);
    sources.unauthorized = nullptr;
    EXPECT_TRUE(starts_with(wsrv::admin_response("GET /metrics HTTP/1.1\r\n\r\n", sources),
                            "HTTP/1.1 401 Unauthorized\r\n"));
}

// Without a token nothing about the headers is read: the same function, the same bytes as before
// this ADR, for everyone who does not set the key.
TEST(AdminHttpToken, WithoutATokenNoHeaderIsEverLookedAt) {
    for (const std::string_view head :
         {std::string_view{"GET /metrics HTTP/1.1\r\nAuthorization: garbage\r\n\r\n"},
          std::string_view{"GET /metrics HTTP/1.1\r\nAuthorization: Bearer a\r\nAuthorization: "
                           "Bearer b\r\n\r\n"},
          std::string_view{"GET /metrics HTTP/1.1\r\nX: a\x01"
                           "b\r\n\r\n"}}) {
        EXPECT_TRUE(starts_with(answer(head), "HTTP/1.1 200 OK\r\n")) << head;
    }
}

}  // namespace
