// SPDX-License-Identifier: Apache-2.0
#include "base/ring_buffer.hpp"

#include <algorithm>

namespace wsrv {

std::size_t RingBuffer::write(ByteView data) noexcept {
    const std::size_t accepted = std::min(data.size(), capacity() - size_);
    std::size_t tail = (head_ + size_) % std::max<std::size_t>(capacity(), 1);
    std::size_t done = 0;
    while (done < accepted) {
        const std::size_t run = std::min(accepted - done, capacity() - tail);
        std::copy_n(data.begin() + static_cast<std::ptrdiff_t>(done),
                    run,
                    storage_.begin() + static_cast<std::ptrdiff_t>(tail));
        done += run;
        tail = (tail + run) % capacity();
    }
    size_ += accepted;
    return accepted;
}

std::size_t RingBuffer::read(MutableByteView out) noexcept {
    const std::size_t count = std::min(out.size(), size_);
    std::size_t done = 0;
    while (done < count) {
        const ByteView run = contiguous_front();
        const std::size_t take = std::min(count - done, run.size());
        std::copy_n(run.begin(), take, out.begin() + static_cast<std::ptrdiff_t>(done));
        consume(take);
        done += take;
    }
    return count;
}

ByteView RingBuffer::contiguous_front() const noexcept {
    if (size_ == 0) {
        return {};
    }
    return ByteView{storage_}.subspan(head_, std::min(size_, capacity() - head_));
}

void RingBuffer::consume(std::size_t count) noexcept {
    WSRV_ASSERT(count <= size_);
    if (count == 0) {
        return;
    }
    head_ = (head_ + count) % capacity();
    size_ -= count;
}

}  // namespace wsrv
