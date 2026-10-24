// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace wsrv {

/// Streaming SHA-1 (RFC 3174). Used ONLY for Sec-WebSocket-Accept (RFC 6455 section 4.2.2), which
/// needs SHA-1 by protocol definition and no collision resistance. Not for general-purpose security
/// use. Thread-affinity: a Sha1 object is not synchronised; distinct objects are independent.
class Sha1 {
public:
    using Digest = std::array<std::byte, 20>;

    Sha1() noexcept {
        reset();
    }

    /// Add `data` to the message. Any chunking of the same bytes gives the same digest.
    void update(ByteView data) noexcept;

    /// Finish the message and return its digest. The object is then reset and can hash a new
    /// message.
    [[nodiscard]] Digest finish() noexcept;

    /// One-shot convenience.
    [[nodiscard]] static Digest hash(ByteView data) noexcept;

private:
    void reset() noexcept;
    void process_block(ByteView block) noexcept;  // block.size() == 64

    std::array<std::uint32_t, 5> state_{};
    std::array<std::byte, 64> block_{};
    std::size_t block_len_ = 0;      // bytes currently buffered in block_
    std::uint64_t total_bytes_ = 0;  // message length so far (wraps only after 2^64 bytes = 16 EiB)
};

}  // namespace wsrv
