// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"

#include <cstddef>
#include <cstdint>

namespace wsrv {

/// Incremental, strict UTF-8 validator (RFC 3629; Unicode Table 3-7). Rejects overlong encodings,
/// surrogates (U+D800..U+DFFF), code points above U+10FFFF and stray continuation bytes.
///
/// Designed for WebSocket text messages (RFC 6455 section 8.1): bytes arrive in arbitrary chunks, a
/// multi-byte character may be split across chunks or fragments, and the connection must fail at
/// the FIRST invalid byte instead of after the final fragment (EC-U06).
///
/// Thread-affinity: not synchronised; use one validator per message stream.
class Utf8Validator {
public:
    /// Feed the next chunk. @return Error{InvalidUtf8} as soon as a byte makes the stream invalid;
    /// afterwards the validator stays failed (every feed() fails) until reset(). A truncated
    /// character at the end of the chunk is NOT an error here: the next chunk may complete it;
    /// check at_boundary() when the message ends.
    [[nodiscard]] Result<void> feed(ByteView chunk) noexcept;

    /// True if everything fed so far is a complete, valid sequence (no character pending, not
    /// failed).
    [[nodiscard]] bool at_boundary() const noexcept {
        return !failed_ && remaining_ == 0;
    }

    /// Number of bytes (0..3) of an unfinished character consumed so far. A streaming consumer
    /// holds these back so each delivered part ends on a character boundary.
    [[nodiscard]] std::size_t pending_bytes() const noexcept {
        return failed_ ? 0 : seen_;
    }

    /// Forget all state; ready for a new message.
    void reset() noexcept;

private:
    [[nodiscard]] bool step(std::uint8_t byte) noexcept;
    [[nodiscard]] bool start_sequence(std::uint8_t lead) noexcept;

    std::uint8_t remaining_ = 0;  // continuation bytes still expected
    std::uint8_t seen_ = 0;       // bytes of the current sequence consumed (including the lead)
    std::uint8_t lo_ = 0x80;  // allowed range of the NEXT byte; only the second byte of a sequence
    std::uint8_t hi_ = 0xBF;  // has a range other than 80..BF
    bool failed_ = false;
};

/// One-shot check of a complete string: valid AND no character left pending.
[[nodiscard]] bool is_valid_utf8(ByteView data) noexcept;

}  // namespace wsrv
