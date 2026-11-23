// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the head of a publish request (ADR-0028 amendment 1). Whatever the bytes are,
// the interpreter never crashes, answers only with a documented status, and whatever it accepts has
// exactly one unambiguous way to find the end of the body: one canonical Content-Length, no
// Transfer-Encoding, no Expect, one credential, and a topic that carries no escape or separator.
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "http/publish_request.hpp"
#include "http/request_parser.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const wsrv::ByteView all = std::as_bytes(std::span<const std::uint8_t>{data, size});
    // The endpoint gives the head at most 4 KiB; the same bound keeps this target fast.
    wsrv::HttpRequestParser parser{
        wsrv::HttpLimits{.max_request_line = 2048, .max_total_bytes = 4096}};
    const auto progress = parser.feed(all);
    if (!progress || !progress->complete) {
        return 0;
    }
    const wsrv::HttpRequest& head = parser.request();
    const auto result = wsrv::interpret_publish(head);
    if (!result) {
        const int status = result.error().status;
        WSRV_ASSERT(status == 401 || status == 404 || status == 405 || status == 411 ||
                    status == 417 || status == 501 || status == 400);
        return 0;
    }
    // An accepted request: one way to find the body, one credential, nothing smuggled.
    WSRV_ASSERT(head.method == "POST");
    WSRV_ASSERT(head.count("Transfer-Encoding") == 0);
    WSRV_ASSERT(head.count("Expect") == 0);
    WSRV_ASSERT(head.count("Authorization") == 1);
    const auto lengths = head.values("Content-Length");
    WSRV_ASSERT(lengths.size() == 1);
    WSRV_ASSERT(std::to_string(result->content_length) == std::string{lengths.front()});
    WSRV_ASSERT(!result->topic.empty());
    WSRV_ASSERT(std::ranges::none_of(result->topic, [](char c) {
        return c == '%' || c == '&' || c == '=' || c == '?' || c == '#' || c <= ' ';
    }));
    WSRV_ASSERT(!result->bearer.empty());
    WSRV_ASSERT(std::ranges::none_of(result->bearer, [](char c) { return c <= ' ' || c == 0x7f; }));
    return 0;
}
