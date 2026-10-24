// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"

#include <cstddef>
#include <vector>

namespace wsrv {

/// A fixed-capacity circular byte queue: constant memory, no reallocation, O(1) per byte. Writes
/// accept only what fits (the caller decides what to do with the rest: backpressure, ADR-0007).
/// Thread-affinity: loop-thread-only (not synchronised).
class RingBuffer {
public:
    explicit RingBuffer(std::size_t capacity) : storage_(capacity) {}

    /// Copy in as many bytes of `data` as fit. @return the number accepted (0..data.size()).
    [[nodiscard]] std::size_t write(ByteView data) noexcept;

    /// Copy out up to out.size() bytes and remove them. @return the number copied.
    [[nodiscard]] std::size_t read(MutableByteView out) noexcept;

    /// The longest run of stored bytes that is contiguous in memory (for a single write() syscall).
    /// May be shorter than size() when the data wraps; consume() it and ask again.
    [[nodiscard]] ByteView contiguous_front() const noexcept;

    /// Drop the first `count` bytes. Precondition: count <= size().
    void consume(std::size_t count) noexcept;

    [[nodiscard]] std::size_t size() const noexcept {
        return size_;
    }
    [[nodiscard]] std::size_t capacity() const noexcept {
        return storage_.size();
    }
    [[nodiscard]] bool empty() const noexcept {
        return size_ == 0;
    }
    [[nodiscard]] bool full() const noexcept {
        return size_ == storage_.size();
    }

private:
    std::vector<std::byte> storage_;
    std::size_t head_ = 0;  // index of the oldest byte
    std::size_t size_ = 0;
};

}  // namespace wsrv
