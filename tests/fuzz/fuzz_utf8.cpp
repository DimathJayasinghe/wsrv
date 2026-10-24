// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: incremental UTF-8 validation must agree with itself however the input is
// chunked.
#include "base/assert.hpp"
#include "protocol/utf8.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const wsrv::ByteView all = std::as_bytes(std::span<const std::uint8_t>{data, size});
    const bool whole = wsrv::is_valid_utf8(all);

    // Property 1: the verdict does not depend on where the input is split into two chunks.
    const std::size_t split = size == 0 ? 0 : static_cast<std::size_t>(data[0]) % (size + 1);
    wsrv::Utf8Validator two_chunks;
    const bool chunked = two_chunks.feed(all.first(split)).has_value() &&
                         two_chunks.feed(all.subspan(split)).has_value() &&
                         two_chunks.at_boundary();
    WSRV_ASSERT(chunked == whole);

    // Property 2: ...nor on feeding one byte at a time. Also: a valid complete message has nothing
    // pending.
    wsrv::Utf8Validator bytewise;
    bool bytewise_ok = true;
    for (std::size_t i = 0; i < size && bytewise_ok; ++i) {
        bytewise_ok = bytewise.feed(all.subspan(i, 1)).has_value();
    }
    bytewise_ok = bytewise_ok && bytewise.at_boundary();
    WSRV_ASSERT(bytewise_ok == whole);
    if (whole) {
        WSRV_ASSERT(bytewise.pending_bytes() == 0);
    }
    return 0;
}
