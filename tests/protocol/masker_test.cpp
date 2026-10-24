// SPDX-License-Identifier: Apache-2.0
#include "protocol/masker.hpp"

#include "base/bytes.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <random>
#include <vector>

namespace {

using wsrv::MaskKey;

/// The definition from RFC 6455 section 5.3, byte by byte. The fast implementation must equal it.
void naive_mask(std::vector<std::byte>& data, MaskKey key, std::size_t start) {
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] ^= key[(start + i) % 4];
    }
}

std::vector<std::byte> pattern(std::size_t size, std::uint32_t seed) {
    std::mt19937 rng{seed};
    std::vector<std::byte> out(size);
    for (std::byte& b : out) {
        b = static_cast<std::byte>(rng() & 0xFFU);
    }
    return out;
}

constexpr MaskKey kRfcKey{std::byte{0x37}, std::byte{0xfa}, std::byte{0x21}, std::byte{0x3d}};

TEST(Masker, Rfc6455SampleUnmasksToHello) {  // RFC 6455 section 5.7
    std::array<std::byte, 5> payload{
        std::byte{0x7f}, std::byte{0x9f}, std::byte{0x4d}, std::byte{0x51}, std::byte{0x58}};
    wsrv::apply_mask(payload, kRfcKey);
    EXPECT_EQ(wsrv::as_chars(payload), "Hello");
}

TEST(Masker, ZeroKeyIsTheIdentityAndAllOnesInverts) {
    std::vector<std::byte> data = pattern(37, 1);
    const std::vector<std::byte> original = data;
    wsrv::apply_mask(data, MaskKey{});
    EXPECT_EQ(data, original);
    wsrv::apply_mask(data,
                     MaskKey{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}});
    for (std::size_t i = 0; i < data.size(); ++i) {
        EXPECT_EQ(data[i], ~original[i]);
    }
}

TEST(Masker, ApplyingTwiceRestoresTheData) {
    std::vector<std::byte> data = pattern(1000, 2);
    const std::vector<std::byte> original = data;
    wsrv::apply_mask(data, kRfcKey, 3);
    EXPECT_NE(data, original);
    wsrv::apply_mask(data, kRfcKey, 3);
    EXPECT_EQ(data, original);
}

TEST(Masker, FastPathEqualsTheDefinitionForEveryLengthOffsetAndAlignment) {
    const std::array<MaskKey, 4> keys{
        kRfcKey,
        MaskKey{},
        MaskKey{std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}, std::byte{0xFF}},
        MaskKey{std::byte{1}, std::byte{2}, std::byte{3}, std::byte{4}}};
    for (const MaskKey& key : keys) {
        for (std::size_t length = 0; length <= 130; ++length) {
            for (std::size_t start = 0; start < 8; ++start) {
                for (std::size_t misalign = 0; misalign < 8;
                     misalign += 3) {  // 0, 3, 6: unaligned buffers
                    std::vector<std::byte> backing =
                        pattern(length + 16, static_cast<std::uint32_t>(length));
                    std::vector<std::byte> expected(
                        backing.begin() + static_cast<std::ptrdiff_t>(misalign),
                        backing.begin() + static_cast<std::ptrdiff_t>(misalign + length));
                    naive_mask(expected, key, start);
                    wsrv::apply_mask(
                        wsrv::MutableByteView{backing}.subspan(misalign, length), key, start);
                    ASSERT_TRUE(std::equal(expected.begin(),
                                           expected.end(),
                                           backing.begin() + static_cast<std::ptrdiff_t>(misalign)))
                        << "length " << length << " start " << start << " misalign " << misalign;
                }
            }
        }
    }
}

TEST(Masker, BytesOutsideTheViewAreUntouched) {
    std::vector<std::byte> backing = pattern(64, 5);
    const std::vector<std::byte> original = backing;
    wsrv::apply_mask(wsrv::MutableByteView{backing}.subspan(10, 30), kRfcKey);
    for (std::size_t i = 0; i < backing.size(); ++i) {
        if (i < 10 || i >= 40) {
            EXPECT_EQ(backing[i], original[i]) << i;
        }
    }
}

TEST(Masker, RunningMaskerContinuesAcrossChunksWhereverTheyAreCut) {  // EC-F17
    const std::vector<std::byte> original = pattern(500, 7);
    std::vector<std::byte> whole = original;
    wsrv::apply_mask(whole, kRfcKey);

    std::mt19937 rng{99};
    for (int round = 0; round < 200; ++round) {
        std::vector<std::byte> chunked = original;
        wsrv::Masker masker{kRfcKey};
        for (std::size_t pos = 0; pos < chunked.size();) {
            const std::size_t take = std::min<std::size_t>(rng() % 23, chunked.size() - pos);
            masker.apply(wsrv::MutableByteView{chunked}.subspan(pos, take));
            pos += take;
        }
        ASSERT_EQ(chunked, whole) << "round " << round;
    }
}

TEST(Masker, OffsetTracksTheBytesProcessed) {
    wsrv::Masker masker{kRfcKey};
    std::vector<std::byte> data(7);
    masker.apply(data);
    EXPECT_EQ(masker.offset(), 7U);
    masker.apply(wsrv::MutableByteView{});
    EXPECT_EQ(masker.offset(), 7U);
}

}  // namespace
