// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: the permessage-deflate inflater (ADR-0020 section 4) must stay bounded and
// deterministic on arbitrary bytes. Per call it never writes past the caller's buffer or reads
// past the input, and the result (the inflated bytes, or "invalid") must not depend on how the
// input is chunked or how small the output buffer is (the bytes produced before a failure
// excepted: the caller drops the message then). Input layout: byte 0 selects the chunking,
// the output buffer and the takeover mode; the rest is the compressed message.
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "extensions/zlib_codec.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace {

// A hostile stream can expand a thousand-fold: stop asking once this much has been produced, as
// the real caller does at its message limit.
constexpr std::size_t kOutputCap = std::size_t{1} << 20U;

struct Outcome {
    std::vector<std::byte> bytes;
    bool invalid = false;
    bool capped = false;
};

Outcome run(wsrv::ByteView input, std::size_t in_step, std::size_t out_size, bool takeover_off) {
    wsrv::DeflateParams params;
    params.client_no_context_takeover = takeover_off;
    const wsrv::ZlibCodecFactory factory{wsrv::ZlibSettings{}};
    auto codecs = factory.create(params);
    WSRV_ASSERT(codecs.has_value());

    Outcome outcome;
    std::vector<std::byte> buffer(out_size);
    const auto take = [&](std::size_t produced) {
        WSRV_ASSERT(produced <= buffer.size());
        outcome.bytes.insert(outcome.bytes.end(),
                             buffer.begin(),
                             buffer.begin() + static_cast<std::ptrdiff_t>(produced));
        if (outcome.bytes.size() >= kOutputCap) {
            outcome.capped = true;
        }
    };
    while (!input.empty() && !outcome.capped) {
        wsrv::ByteView rest = input.first(std::min(in_step, input.size()));
        input = input.subspan(rest.size());
        while (!outcome.capped) {
            const auto step = codecs->inflater->inflate(rest, buffer);
            if (!step) {
                outcome.invalid = true;
                return outcome;
            }
            WSRV_ASSERT(step->consumed <= rest.size());
            take(step->produced);
            rest = rest.subspan(step->consumed);
            if (rest.empty() && step->produced < buffer.size()) {
                break;
            }
        }
    }
    while (!outcome.capped) {
        const auto step = codecs->inflater->finish(buffer);
        if (!step) {
            outcome.invalid = true;
            return outcome;
        }
        take(step->produced);
        if (step->done) {
            break;
        }
    }
    return outcome;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    if (size == 0) {
        return 0;
    }
    const std::uint8_t selector = data[0];
    const wsrv::ByteView input = std::as_bytes(std::span{data + 1, size - 1});
    const std::size_t in_step = std::size_t{1} << (selector & 7U);                  // 1 .. 128
    const std::size_t out_size = std::size_t{1} << (((selector >> 3U) & 7U) + 3U);  // 8 .. 1024
    const bool takeover_off = (selector & 0x40U) != 0;

    const Outcome whole = run(input, input.size() + 1, 16 * 1024, takeover_off);
    const Outcome pieces = run(input, in_step, out_size, takeover_off);
    if (!whole.capped && !pieces.capped) {
        WSRV_ASSERT(whole.invalid == pieces.invalid);
        if (!whole.invalid) {  // after a failure, the bytes produced before it depend on the
                               // chunking
            WSRV_ASSERT(whole.bytes == pieces.bytes);
        }
    } else {
        const std::size_t common = std::min(whole.bytes.size(), pieces.bytes.size());
        WSRV_ASSERT(std::equal(whole.bytes.begin(),
                               whole.bytes.begin() + static_cast<std::ptrdiff_t>(common),
                               pieces.bytes.begin()));
    }
    return 0;
}
