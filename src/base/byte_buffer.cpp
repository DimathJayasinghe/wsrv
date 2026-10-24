// SPDX-License-Identifier: Apache-2.0
#include "base/byte_buffer.hpp"

#include "base/checked.hpp"

#include <algorithm>

namespace wsrv {

Result<void> ByteBuffer::append(ByteView data) {
    const Result<std::size_t> total = checked_add(size(), data.size());
    if (!total.has_value() || *total > max_size_) {
        return fail(ErrorCode::BufferFull, "ByteBuffer bound exceeded");
    }
    if (head_ > 0 && head_ >= size()) {  // at least half of the storage is dead space: compact
        storage_.erase(storage_.begin(), storage_.begin() + static_cast<std::ptrdiff_t>(head_));
        head_ = 0;
    }
    storage_.insert(storage_.end(), data.begin(), data.end());
    return {};
}

ByteView ByteBuffer::view() const noexcept {
    return ByteView{storage_}.subspan(head_);
}

void ByteBuffer::consume(std::size_t count) noexcept {
    WSRV_ASSERT(count <= size());
    head_ += count;
    if (head_ == storage_.size()) {
        clear();
    }
}

void ByteBuffer::clear() noexcept {
    storage_.clear();
    head_ = 0;
}

}  // namespace wsrv
