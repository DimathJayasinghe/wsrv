// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the HTTP request parser must give the same answer however the bytes are
// chunked, never crash, and whatever it accepts must be free of injection characters.
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "http/request_parser.hpp"
#include "http/token.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace {

struct Outcome {
    bool complete = false;
    int status = 0;  // 0 = no error
    std::size_t consumed = 0;
    wsrv::HttpRequest request;
};

/// Feed `data` in chunks whose sizes come from `sizes` (cycled); an empty `sizes` means one single
/// chunk.
Outcome run(wsrv::ByteView data, std::span<const std::uint8_t> sizes) {
    wsrv::HttpRequestParser parser;
    Outcome out;
    std::size_t pos = 0;
    std::size_t index = 0;
    while (pos < data.size() && !out.complete && out.status == 0) {
        std::size_t take = data.size() - pos;
        if (!sizes.empty()) {
            take = std::min(take, std::size_t{1} + sizes[index++ % sizes.size()] % 11U);
        }
        const auto progress = parser.feed(data.subspan(pos, take));
        if (!progress) {
            out.status = progress.error().status;
            break;
        }
        out.consumed += progress->consumed;
        pos += take;
        out.complete = progress->complete;
    }
    if (out.complete) {
        out.request = parser.request();
    }
    return out;
}

bool same(const Outcome& a, const Outcome& b) {
    if (a.complete != b.complete || a.status != b.status) {
        return false;
    }
    if (!a.complete) {
        return true;
    }
    if (a.consumed != b.consumed || a.request.method != b.request.method ||
        a.request.target != b.request.target ||
        a.request.version_minor != b.request.version_minor ||
        a.request.headers.size() != b.request.headers.size()) {
        return false;
    }
    for (std::size_t i = 0; i < a.request.headers.size(); ++i) {
        if (a.request.headers[i].name != b.request.headers[i].name ||
            a.request.headers[i].value != b.request.headers[i].value) {
            return false;
        }
    }
    return true;
}

bool free_of_injection_characters(std::string_view text) {
    return std::ranges::none_of(text, [](char c) { return c == '\r' || c == '\n' || c == '\0'; });
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const wsrv::ByteView all = std::as_bytes(std::span<const std::uint8_t>{data, size});
    const Outcome whole = run(all, {});

    // Property 1: chunking never changes the outcome (data-derived chunk sizes, and one byte at a
    // time).
    const std::uint8_t one = 0;
    WSRV_ASSERT(same(whole, run(all, std::span<const std::uint8_t>{data, size})));
    WSRV_ASSERT(same(whole, run(all, std::span<const std::uint8_t>{&one, 1} /*size 1 chunks*/)));

    // Property 2: errors are one of the documented statuses.
    WSRV_ASSERT(whole.status == 0 || whole.status == 400 || whole.status == 414 ||
                whole.status == 431 || whole.status == 505);

    // Property 3: an accepted head respects the limits and carries no injection characters.
    if (whole.complete) {
        const wsrv::HttpLimits limits;
        WSRV_ASSERT(whole.consumed <= size && whole.consumed <= limits.max_total_bytes);
        WSRV_ASSERT(whole.request.headers.size() <= limits.max_headers);
        WSRV_ASSERT(wsrv::is_token(whole.request.method));
        WSRV_ASSERT(!whole.request.target.empty() && whole.request.target.front() == '/');
        WSRV_ASSERT(free_of_injection_characters(whole.request.target));
        for (const wsrv::HttpHeader& header : whole.request.headers) {
            WSRV_ASSERT(wsrv::is_token(header.name));
            WSRV_ASSERT(free_of_injection_characters(header.value));
            WSRV_ASSERT(header.value == wsrv::trim_ows(header.value));
        }
    }
    return 0;
}
