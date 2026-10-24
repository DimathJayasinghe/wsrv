// SPDX-License-Identifier: Apache-2.0
#include "crypto/sha1.hpp"

#include <algorithm>

namespace wsrv {

namespace {

constexpr std::size_t kBlockSize = 64;
constexpr std::size_t kLengthOffset =
    56;  // where the 64-bit message length goes in the final block

[[nodiscard]] constexpr std::uint32_t rotl(std::uint32_t value, unsigned bits) noexcept {
    return (value << bits) | (value >> (32U - bits));
}

}  // namespace

void Sha1::reset() noexcept {
    state_ = {0x67452301U, 0xEFCDAB89U, 0x98BADCFEU, 0x10325476U, 0xC3D2E1F0U};
    block_len_ = 0;
    total_bytes_ = 0;
}

void Sha1::process_block(ByteView block) noexcept {
    // Message schedule: 16 words from the block, 64 more derived by rotate-xor.
    std::array<std::uint32_t, 80> w{};
    for (std::size_t t = 0; t < 16; ++t) {
        w[t] = load_be32(block.subspan(4 * t));
    }
    for (std::size_t t = 16; t < 80; ++t) {
        w[t] = rotl(w[t - 3] ^ w[t - 8] ^ w[t - 14] ^ w[t - 16], 1);
    }

    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];

    for (std::size_t t = 0; t < 80; ++t) {
        std::uint32_t f = 0;
        std::uint32_t k = 0;
        if (t < 20) {
            f = (b & c) | (~b & d);
            k = 0x5A827999U;
        } else if (t < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1U;
        } else if (t < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCU;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6U;
        }
        const std::uint32_t temp = rotl(a, 5) + f + e + k + w[t];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = temp;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
}

void Sha1::update(ByteView data) noexcept {
    total_bytes_ += data.size();

    // 1. Top up a partially filled block first.
    if (block_len_ > 0) {
        const std::size_t take = std::min(kBlockSize - block_len_, data.size());
        std::copy_n(data.begin(), take, block_.begin() + static_cast<std::ptrdiff_t>(block_len_));
        block_len_ += take;
        data = data.subspan(take);
        if (block_len_ < kBlockSize) {
            return;
        }
        process_block(block_);
        block_len_ = 0;
    }
    // 2. Whole blocks straight from the input, no copy.
    while (data.size() >= kBlockSize) {
        process_block(data.first(kBlockSize));
        data = data.subspan(kBlockSize);
    }
    // 3. Keep the tail for the next call.
    std::copy(data.begin(), data.end(), block_.begin());
    block_len_ = data.size();
}

Sha1::Digest Sha1::finish() noexcept {
    const std::uint64_t bit_length = total_bytes_ * 8U;

    // Padding: a single 1 bit (0x80), zeros, then the 64-bit big-endian bit length at offset 56.
    block_[block_len_++] = std::byte{0x80};
    if (block_len_ > kLengthOffset) {  // no room for the length: finish this block, use another
        std::fill(
            block_.begin() + static_cast<std::ptrdiff_t>(block_len_), block_.end(), std::byte{0});
        process_block(block_);
        block_len_ = 0;
    }
    std::fill(block_.begin() + static_cast<std::ptrdiff_t>(block_len_), block_.end(), std::byte{0});
    store_be64(MutableByteView{block_}.subspan(kLengthOffset), bit_length);
    process_block(block_);

    Digest digest{};
    for (std::size_t i = 0; i < state_.size(); ++i) {
        store_be32(MutableByteView{digest}.subspan(4 * i), state_[i]);
    }
    reset();
    return digest;
}

Sha1::Digest Sha1::hash(ByteView data) noexcept {
    Sha1 hasher;
    hasher.update(data);
    return hasher.finish();
}

}  // namespace wsrv
