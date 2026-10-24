// SPDX-License-Identifier: Apache-2.0
#include "protocol/masker.hpp"

#include <cstdint>
#include <cstring>

namespace wsrv {

void apply_mask(MutableByteView data, MaskKey key, std::size_t start_offset) noexcept {
    // Rotate the key so that rotated[0] lines up with data[0].
    std::array<std::byte, 8> pattern{};
    for (std::size_t i = 0; i < pattern.size(); ++i) {
        pattern[i] = key[(start_offset + i) % 4];  // two repetitions of the (rotated) key
    }
    std::uint64_t pattern_word = 0;
    std::memcpy(&pattern_word,
                pattern.data(),
                sizeof pattern_word);  // byte-for-byte, endian-neutral XOR below

    std::size_t i = 0;
    for (; i + sizeof pattern_word <= data.size(); i += sizeof pattern_word) {
        std::uint64_t word = 0;
        std::memcpy(
            &word, data.data() + i, sizeof word);  // memcpy: no alignment or aliasing assumptions
        word ^= pattern_word;
        std::memcpy(data.data() + i, &word, sizeof word);
    }
    // 8 is a multiple of the key length, so the tail continues with the same rotation.
    for (; i < data.size(); ++i) {
        data[i] ^= pattern[i % pattern.size()];
    }
}

}  // namespace wsrv
