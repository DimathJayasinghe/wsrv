// SPDX-License-Identifier: Apache-2.0
// libFuzzer target: SHA-1 gives the same digest however the input is split into update() calls.
#include "base/assert.hpp"
#include "crypto/sha1.hpp"

#include <cstddef>
#include <cstdint>
#include <span>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const wsrv::ByteView all = std::as_bytes(std::span<const std::uint8_t>{data, size});
    const wsrv::Sha1::Digest expected = wsrv::Sha1::hash(all);

    // Chunk sizes are taken from the data itself, so the fuzzer explores block-boundary alignments.
    wsrv::Sha1 hasher;
    std::size_t pos = 0;
    std::size_t step = 1;
    while (pos < size) {
        const std::size_t take =
            std::min(size - pos, 1 + static_cast<std::size_t>(data[pos]) % (step * 7));
        hasher.update(all.subspan(pos, take));
        pos += take;
        step = step % 13 + 1;
    }
    WSRV_ASSERT(hasher.finish() == expected);
    return 0;
}
