// SPDX-License-Identifier: Apache-2.0
#include "base/byte_buffer.hpp"

#include "base/checked.hpp"

#include <algorithm>
#include <functional>
#include <new>
#include <stdexcept>

namespace wsrv {

namespace {
constexpr std::size_t kRetainCapacity =
    std::size_t{16} * 1024;  // capacity kept when the buffer drains
}  // namespace

bool ByteBuffer::aliases_storage(ByteView data) const noexcept {
    if (data.empty() || storage_.empty()) {
        return false;
    }
    const std::less<> before;  // std::less gives a total order even for unrelated pointers
    const std::byte* const low = storage_.data();
    const std::byte* const high = low + storage_.size();
    return !before(data.data(), low) && before(data.data(), high);
}

void ByteBuffer::compact() {
    storage_.erase(storage_.begin(), storage_.begin() + static_cast<std::ptrdiff_t>(head_));
    head_ = 0;
}

void ByteBuffer::reserve_for(std::size_t needed) {
    if (needed <= storage_.capacity()) {
        return;
    }
    const Result<std::size_t> doubled = checked_mul<std::size_t>(storage_.capacity(), 2);
    const std::size_t geometric = doubled.has_value() ? *doubled : max_size_;
    storage_.reserve(std::max(needed, std::min(geometric, max_size_)));  // never above the bound
}

Result<void> ByteBuffer::append(ByteView data) noexcept {
    const Result<std::size_t> total = checked_add(size(), data.size());
    if (!total.has_value() || *total > max_size_) {
        return fail(ErrorCode::BufferFull, "ByteBuffer bound exceeded");
    }
    if (data.empty()) {
        return {};
    }
    try {
        std::vector<std::byte> copy;  // used only when `data` points into our own storage
        if (aliases_storage(data)) {
            copy.assign(data.begin(), data.end());
            data = copy;
        }
        // Dead bytes at the front count against the bound: compact before they would push storage
        // past it.
        const Result<std::size_t> raw = checked_add(storage_.size(), data.size());
        if (head_ > 0 && (!raw.has_value() || *raw > max_size_)) {
            compact();
        }
        reserve_for(storage_.size() + data.size());
        storage_.insert(storage_.end(), data.begin(), data.end());
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "ByteBuffer allocation failed");
    } catch (const std::length_error&) {
        return fail(ErrorCode::ResourceExhausted, "ByteBuffer allocation too large");
    }
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
    if (storage_.capacity() > kRetainCapacity) {
        std::vector<std::byte>().swap(
            storage_);  // really release it (shrink_to_fit is only a request)
    }
}

}  // namespace wsrv
