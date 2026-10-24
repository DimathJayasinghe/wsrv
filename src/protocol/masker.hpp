// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"

#include <array>
#include <cstddef>

namespace wsrv {

/// The 4-byte masking key of a client frame (RFC 6455 section 5.3).
using MaskKey = std::array<std::byte, 4>;

/// XOR `data` with the repeating key, as if `data` started `start_offset` bytes into the payload:
/// data[i] ^= key[(start_offset + i) % 4]. Masking and unmasking are the same operation. Equal to
/// the byte-by-byte definition for every length, offset and alignment (differentially tested), but
/// processes 8 bytes at a time. Pure; thread-affinity: any-thread (different buffers).
void apply_mask(MutableByteView data, MaskKey key, std::size_t start_offset = 0) noexcept;

/// Applies a key to a payload that arrives in chunks, remembering the position so the key continues
/// seamlessly across chunk boundaries (EC-F17). Thread-affinity: not synchronised.
class Masker {
public:
    explicit Masker(MaskKey key) noexcept : key_{key} {}

    void apply(MutableByteView chunk) noexcept {
        apply_mask(chunk, key_, offset_ % 4);
        offset_ += chunk.size();
    }

    /// Total bytes processed so far.
    [[nodiscard]] std::size_t offset() const noexcept {
        return offset_;
    }

private:
    MaskKey key_;
    std::size_t offset_ = 0;
};

}  // namespace wsrv
