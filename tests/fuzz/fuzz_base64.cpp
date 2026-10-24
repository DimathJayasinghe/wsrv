// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: strict Base64 never crashes, accepts only canonical text, and round-trips.
#include "base/assert.hpp"
#include "base/bytes.hpp"
#include "crypto/base64.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const wsrv::ByteView raw = std::as_bytes(std::span<const std::uint8_t>{data, size});

    // Property 1: whatever the decoder ACCEPTS is canonical, so encoding it reproduces the input
    // exactly. (A lenient decoder would break this; it is the executable definition of "strict".)
    const std::string_view text = wsrv::as_chars(raw);
    const wsrv::Result<std::vector<std::byte>> decoded = wsrv::base64_decode(text);
    if (decoded.has_value()) {
        WSRV_ASSERT(wsrv::base64_encode(*decoded) == text);
    }

    // Property 2: encode then decode returns the original bytes.
    const std::string encoded = wsrv::base64_encode(raw);
    const wsrv::Result<std::vector<std::byte>> back = wsrv::base64_decode(encoded);
    WSRV_ASSERT(back.has_value());
    WSRV_ASSERT(std::equal(back->begin(), back->end(), raw.begin(), raw.end()));
    return 0;
}
