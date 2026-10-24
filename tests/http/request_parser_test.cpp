// SPDX-License-Identifier: Apache-2.0
#include "http/request_parser.hpp"

#include "base/bytes.hpp"

#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <vector>

namespace {

using wsrv::HttpLimits;
using wsrv::HttpRequestParser;

constexpr std::string_view kValid = "GET /chat?room=1 HTTP/1.1\r\n"
                                    "Host: example.com\r\n"
                                    "Upgrade: websocket\r\n"
                                    "Connection: keep-alive, Upgrade\r\n"
                                    "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
                                    "Sec-WebSocket-Version: 13\r\n"
                                    "Origin: https://app.example.com\r\n"
                                    "\r\n";

struct Outcome {
    bool complete = false;
    int status = 0;            // 0 = no error
    std::size_t consumed = 0;  // total bytes consumed
    wsrv::HttpRequest request;
};

/// Feed `text` in chunks of `chunk` bytes (0 = all at once). Stops at completion or the first
/// error.
Outcome run(std::string_view text, std::size_t chunk = 0, HttpLimits limits = {}) {
    HttpRequestParser parser{limits};
    Outcome out;
    const std::size_t step = chunk == 0 ? text.size() : chunk;
    std::size_t pos = 0;
    while (pos < text.size() && !out.complete && out.status == 0) {
        const std::size_t take = std::min(step, text.size() - pos);
        const auto progress = parser.feed(wsrv::as_bytes(text.substr(pos, take)));
        if (!progress) {
            out.status = progress.error().status;
            break;
        }
        out.consumed += progress->consumed;
        pos += take;
        out.complete = progress->complete;
        if (out.complete) {
            break;
        }
    }
    if (out.complete) {
        out.request = parser.request();
    }
    return out;
}

// ---- the happy path
// ------------------------------------------------------------------------------------

TEST(HttpRequestParser, ParsesAValidUpgradeRequest) {
    const Outcome out = run(kValid);
    ASSERT_TRUE(out.complete);
    EXPECT_EQ(out.consumed, kValid.size());
    EXPECT_EQ(out.request.method, "GET");
    EXPECT_EQ(out.request.target, "/chat?room=1");
    EXPECT_EQ(out.request.version_minor, 1);
    ASSERT_EQ(out.request.headers.size(), 6U);
    EXPECT_EQ(out.request.values("connection"),
              (std::vector<std::string_view>{"keep-alive, Upgrade"}));
    EXPECT_EQ(out.request.values("Origin"),
              (std::vector<std::string_view>{"https://app.example.com"}));
}

TEST(HttpRequestParser, ByteAtATimeGivesTheSameRequest) {  // EC-H17
    const Outcome whole = run(kValid);
    const Outcome slow = run(kValid, 1);
    ASSERT_TRUE(slow.complete);
    EXPECT_EQ(slow.consumed, kValid.size());
    EXPECT_EQ(slow.request.target, whole.request.target);
    ASSERT_EQ(slow.request.headers.size(), whole.request.headers.size());
    for (std::size_t i = 0; i < whole.request.headers.size(); ++i) {
        EXPECT_EQ(slow.request.headers[i].name, whole.request.headers[i].name);
        EXPECT_EQ(slow.request.headers[i].value, whole.request.headers[i].value);
    }
}

TEST(HttpRequestParser, EverySplitPointGivesTheSameResult) {  // EC-H18, including inside \r\n\r\n
    const Outcome whole = run(kValid);
    ASSERT_TRUE(whole.complete);
    for (std::size_t split = 1; split < kValid.size(); ++split) {
        HttpRequestParser parser;
        const auto first = parser.feed(wsrv::as_bytes(kValid.substr(0, split)));
        ASSERT_TRUE(first.has_value()) << split;
        ASSERT_FALSE(first->complete) << split;
        const auto second = parser.feed(wsrv::as_bytes(kValid.substr(split)));
        ASSERT_TRUE(second.has_value()) << split;
        ASSERT_TRUE(second->complete) << split;
        EXPECT_EQ(first->consumed + second->consumed, kValid.size()) << split;
        // Every byte must arrive intact: compare the complete request with the one-shot parse.
        const wsrv::HttpRequest& got = parser.request();
        ASSERT_EQ(got.method, whole.request.method) << split;
        ASSERT_EQ(got.target, whole.request.target) << split;
        ASSERT_EQ(got.version_minor, whole.request.version_minor) << split;
        ASSERT_EQ(got.headers.size(), whole.request.headers.size()) << split;
        for (std::size_t i = 0; i < got.headers.size(); ++i) {
            ASSERT_EQ(got.headers[i].name, whole.request.headers[i].name)
                << split << " header " << i;
            ASSERT_EQ(got.headers[i].value, whole.request.headers[i].value)
                << split << " header " << i;
        }
    }
}

TEST(HttpRequestParser, BytesAfterTheBlankLineAreNotConsumed) {  // EC-H19
    const std::string with_frame =
        std::string{kValid} + "\x81\x85\x37\xfa\x21\x3d\x7f\x9f\x4d\x51\x58";
    HttpRequestParser parser;
    const auto progress = parser.feed(wsrv::as_bytes(with_frame));
    ASSERT_TRUE(progress.has_value());
    EXPECT_TRUE(progress->complete);
    EXPECT_EQ(progress->consumed, kValid.size());  // the caller owns the 11 frame bytes that follow
}

TEST(HttpRequestParser, TrimsValueWhitespaceAndKeepsDuplicates) {
    const Outcome out = run("GET / HTTP/1.1\r\nA:   x  y \t\r\nA: second\r\nEmpty:\r\n\r\n");
    ASSERT_TRUE(out.complete);
    EXPECT_EQ(out.request.values("a"), (std::vector<std::string_view>{"x  y", "second"}));
    EXPECT_EQ(out.request.values("empty"), (std::vector<std::string_view>{""}));
}

TEST(HttpRequestParser, AcceptsObsTextInValuesButNotInNames) {
    EXPECT_TRUE(run("GET / HTTP/1.1\r\nX: caf\xC3\xA9\r\n\r\n").complete);
    EXPECT_EQ(run("GET / HTTP/1.1\r\nX\xC3\xA9: v\r\n\r\n").status, 400);
}

TEST(HttpRequestParser, AcceptsTheCharactersBrowsersSendRawInUrls) {
    // WHATWG URL: browsers percent-encode only controls, space, " < > # (and a few more), so these
    // arrive raw.
    for (const char* target : {"/chat?ids=[1,2]",
                               "/chat?q={a}",
                               "/chat?a|b",
                               "/a^b",
                               "/a`b",
                               "/%7Euser/;p?b=c&d=e",
                               "/chat?x=a:b@c",
                               "/"}) {
        const std::string text = std::string{"GET "} + target + " HTTP/1.1\r\n\r\n";
        const Outcome out = run(text);
        EXPECT_TRUE(out.complete) << target;
        EXPECT_EQ(out.request.target, target);
    }
}

TEST(HttpRequestParser, StillRejectsTheCharactersThatAreUnsafeInATarget) {
    for (const char* target : {"/a b", "/a\"b", "/a<b", "/a>b", "/a#b", "/a\\b"}) {
        const std::string text = std::string{"GET "} + target + " HTTP/1.1\r\n\r\n";
        EXPECT_EQ(run(text).status, 400) << target;
    }
}

TEST(HttpRequestParser, ParsesHttp10WithMinorZero) {
    const Outcome out = run("GET / HTTP/1.0\r\n\r\n");
    ASSERT_TRUE(out.complete);
    EXPECT_EQ(out.request.version_minor, 0);
}

// ---- malformed input is rejected, identically however it is chunked
// -------------------------------------

struct BadCase {
    std::string text;
    int status;
    const char* why;
};

std::vector<BadCase> bad_cases() {
    return {
        // EC-H15: line endings and control characters
        {"GET / HTTP/1.1\nHost: x\r\n\r\n", 400, "bare LF in request line"},
        {"GET / HTTP/1.1\r\nHost: x\n\r\n", 400, "bare LF in header"},
        {"GET / HTTP/1.1\r\nHost: x\r\n\n", 400, "bare LF as blank line"},
        {"GET / HTTP/1.1\rHost: x\r\n\r\n", 400, "bare CR then data"},
        {"GET / HTTP/1.1\r\nHo\rst: x\r\n\r\n", 400, "bare CR inside a header name"},
        {std::string{"GET / HTTP/1.1\r\nHost: a\0b\r\n\r\n", 29}, 400, "NUL in value"},
        {"GET / HTTP/1.1\r\nHost: a\x01z\r\n\r\n", 400, "control character in value"},
        {"GET / HTTP/1.1\r\nHost: a\x7f\r\n\r\n", 400, "DEL in value"},
        {"GET / HTTP/1.1\r\nHost: x\r\n folded\r\n\r\n", 400, "obs-fold with space"},
        {"GET / HTTP/1.1\r\nHost: x\r\n\tfolded\r\n\r\n", 400, "obs-fold with tab"},
        {"GET / HTTP/1.1\r\nHost : x\r\n\r\n", 400, "space before colon"},
        {"GET / HTTP/1.1\r\n: x\r\n\r\n", 400, "empty header name"},
        {"GET / HTTP/1.1\r\nBad Name: x\r\n\r\n", 400, "space inside header name"},
        {"GET / HTTP/1.1\r\nBad(Name): x\r\n\r\n", 400, "separator in header name"},
        {"GET / HTTP/1.1\r\nNoColonHere\r\n\r\n", 400, "header without colon"},
        {"\r\nGET / HTTP/1.1\r\n\r\n", 400, "leading empty line"},
        // request line shape
        {"GET  / HTTP/1.1\r\n\r\n", 400, "double space"},
        {"GET /\r\n\r\n", 400, "missing version"},
        {"GET / HTTP/1.1 extra\r\n\r\n", 400, "extra field"},
        {"GET\t/ HTTP/1.1\r\n\r\n", 400, "tab as separator"},
        {"G(T / HTTP/1.1\r\n\r\n", 400, "method with a separator"},
        // EC-H23: request target forms
        {"GET http://example.com/ HTTP/1.1\r\n\r\n", 400, "absolute-URI"},
        {"GET * HTTP/1.1\r\n\r\n", 400, "asterisk form"},
        {"GET example.com:80 HTTP/1.1\r\n\r\n", 400, "authority form"},
        {"GET /a#frag HTTP/1.1\r\n\r\n", 400, "fragment"},
        {"GET /a%zz HTTP/1.1\r\n\r\n", 400, "invalid percent-encoding"},
        {"GET /a%4 HTTP/1.1\r\n\r\n", 400, "truncated percent-encoding"},
        {"GET /a b HTTP/1.1\r\n\r\n", 400, "space in target"},
        {"GET /a\"b HTTP/1.1\r\n\r\n", 400, "quote in target"},
        {"GET /caf\xC3\xA9 HTTP/1.1\r\n\r\n", 400, "raw non-ASCII in target"},
        // versions
        {"GET / http/1.1\r\n\r\n", 400, "lowercase version"},
        {"GET / HTTP/1.\r\n\r\n", 400, "truncated version"},
        {"GET / HTTP/11\r\n\r\n", 400, "version without dot"},
        {"GET / HTTP/1.x\r\n\r\n", 400, "non-digit minor"},
        {"GET / HTTP/2.0\r\n\r\n", 505, "unsupported major version"},
        {"GET / HTTP/0.9\r\n\r\n", 505, "unsupported major version 0"},
    };
}

TEST(HttpRequestParser, RejectsMalformedRequestsWithTheExpectedStatus) {
    for (const BadCase& c : bad_cases()) {
        const Outcome whole = run(c.text);
        EXPECT_FALSE(whole.complete) << c.why;
        EXPECT_EQ(whole.status, c.status) << c.why;
    }
}

TEST(HttpRequestParser, MalformedRequestsAreRejectedIdenticallyWhenChunked) {
    for (const BadCase& c : bad_cases()) {
        for (const std::size_t chunk :
             {std::size_t{1}, std::size_t{2}, std::size_t{3}, std::size_t{7}}) {
            const Outcome out = run(c.text, chunk);
            EXPECT_FALSE(out.complete) << c.why << " chunk " << chunk;
            EXPECT_EQ(out.status, c.status) << c.why << " chunk " << chunk;
        }
    }
}

TEST(HttpRequestParser, BareCrAtTheEndOfAChunkIsCaughtByTheNextChunk) {
    HttpRequestParser parser;
    ASSERT_TRUE(parser.feed(wsrv::as_bytes("GET / HTTP/1.1\r")).has_value());
    const auto next = parser.feed(wsrv::as_bytes("X"));
    ASSERT_FALSE(next.has_value());
    EXPECT_EQ(next.error().status, 400);
}

TEST(HttpRequestParser, StaysFailedAndStaysComplete) {
    HttpRequestParser failing;
    ASSERT_FALSE(failing.feed(wsrv::as_bytes("GET / HTTP/1.1\nX")).has_value());
    EXPECT_TRUE(failing.failed());
    EXPECT_FALSE(failing.feed(wsrv::as_bytes(kValid)).has_value());  // valid data does not heal it

    HttpRequestParser done;
    ASSERT_TRUE(done.feed(wsrv::as_bytes(kValid)).has_value());
    const auto again = done.feed(wsrv::as_bytes("more"));
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->consumed, 0U);
    EXPECT_TRUE(again->complete);
}

TEST(HttpRequestParser, ResetAllowsReuse) {
    HttpRequestParser parser;
    ASSERT_FALSE(parser.feed(wsrv::as_bytes("GET / HTTP/1.1\nX")).has_value());
    parser.reset();
    EXPECT_FALSE(parser.failed());
    const auto progress = parser.feed(wsrv::as_bytes(kValid));
    ASSERT_TRUE(progress.has_value());
    EXPECT_TRUE(progress->complete);
}

// ---- EC-H14: limits are exact
// ---------------------------------------------------------------------------

std::string request_line_of_length(std::size_t length) {
    const std::string prefix = "GET /";
    const std::string suffix = " HTTP/1.1";
    return prefix + std::string(length - prefix.size() - suffix.size(), 'a') + suffix;
}

TEST(HttpRequestParser, RequestLineAtTheLimitIsAcceptedAndOneOverIs414) {
    EXPECT_TRUE(run(request_line_of_length(4096) + "\r\n\r\n").complete);
    EXPECT_EQ(run(request_line_of_length(4097) + "\r\n\r\n").status, 414);
    EXPECT_EQ(run(request_line_of_length(4097) + "\r\n\r\n", 1).status, 414);
}

TEST(HttpRequestParser, TotalSizeAtTheLimitIsAcceptedAndOneOverIs431) {
    const std::string head = "GET / HTTP/1.1\r\nX: ";
    const std::string tail = "\r\n\r\n";
    const std::string exact = head + std::string(8192 - head.size() - tail.size(), 'v') + tail;
    ASSERT_EQ(exact.size(), 8192U);
    EXPECT_TRUE(run(exact).complete);
    const std::string over = head + std::string(8192 - head.size() - tail.size() + 1, 'v') + tail;
    EXPECT_EQ(run(over).status, 431);
    EXPECT_EQ(run(over, 1).status, 431);
}

TEST(HttpRequestParser, HeaderCountAtTheLimitIsAcceptedAndOneOverIs431) {
    std::string at_limit = "GET / HTTP/1.1\r\n";
    for (int i = 0; i < 64; ++i) {
        at_limit += "H" + std::to_string(i) + ": v\r\n";
    }
    EXPECT_TRUE(run(at_limit + "\r\n").complete);
    EXPECT_EQ(run(at_limit + "H64: v\r\n\r\n").status, 431);
}

TEST(HttpRequestParser, AHugeHeaderIsCutOffNearTheLimitWithoutBufferingIt) {
    // 1 MiB of 'a' as one header value: the parser must fail around 8 KiB, not after reading it
    // all.
    const std::string huge = "GET / HTTP/1.1\r\nX: " + std::string(1 << 20, 'a') + "\r\n\r\n";
    HttpRequestParser parser;
    std::size_t fed = 0;
    int status = 0;
    while (fed < huge.size() && status == 0) {
        const auto progress = parser.feed(wsrv::as_bytes(std::string_view{huge}.substr(fed, 1000)));
        fed += 1000;
        if (!progress) {
            status = progress.error().status;
        }
    }
    EXPECT_EQ(status, 431);
    EXPECT_LE(fed, 9000U);  // gave up within one chunk of the 8192-byte limit
}

TEST(HttpRequestParser, CustomLimitsAreHonoured) {
    HttpLimits tiny;
    tiny.max_total_bytes = 40;
    EXPECT_EQ(run(kValid, 0, tiny).status, 431);
    tiny = HttpLimits{};
    tiny.max_headers = 2;
    EXPECT_EQ(run(kValid, 0, tiny).status, 431);
}

}  // namespace
