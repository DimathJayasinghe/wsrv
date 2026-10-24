// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"

#include <cstddef>
#include <vector>

namespace wsrv {

/// A growable byte queue with a HARD upper bound (ADR-0007): data is appended at the back and
/// consumed from the front.
///
/// Memory guarantees (tested, because a peer must not be able to inflate our footprint):
///  - capacity() never exceeds max_size(): consumed bytes at the front are compacted away before
///  the
///    buffer would outgrow the bound, and growth is capped at the bound;
///  - nothing is reserved up front: memory follows the data actually stored;
///  - draining the buffer (consume() of everything, or clear()) releases capacity above 16 KiB, so
///  an
///    idle connection holds almost nothing.
///
/// Thread-affinity: loop-thread-only (not synchronised).
class ByteBuffer {
public:
    explicit ByteBuffer(std::size_t max_size) noexcept : max_size_{max_size} {}

    /// Append `data`, which may be a view of this buffer's own contents (it is copied first).
    /// @return Error{BufferFull} if size()+data.size() would exceed max_size() (or overflow), or
    /// Error{ResourceExhausted} if the allocation fails. On any error the buffer is unchanged. Does
    /// not throw.
    [[nodiscard]] Result<void> append(ByteView data) noexcept;

    /// The stored bytes. Invalidated by append(), consume() and clear().
    [[nodiscard]] ByteView view() const noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return storage_.size() - head_;
    }
    [[nodiscard]] std::size_t max_size() const noexcept {
        return max_size_;
    }

    /// Bytes currently allocated (>= size(), <= max_size()); exposed so tests can verify the memory
    /// bound.
    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.capacity();
    }

    /// Drop the first `count` bytes. Precondition: count <= size().
    void consume(std::size_t count) noexcept;

    /// Drop everything and release large allocations (the bound is kept).
    void clear() noexcept;

private:
    void compact();                        // erase the consumed prefix
    void reserve_for(std::size_t needed);  // grow geometrically, never past max_size_
    [[nodiscard]] bool aliases_storage(ByteView data) const noexcept;

    std::size_t max_size_;
    std::size_t head_ =
        0;  // bytes already consumed at the front of storage_; storage_.size() <= max_size_
    std::vector<std::byte> storage_;
};

}  // namespace wsrv
