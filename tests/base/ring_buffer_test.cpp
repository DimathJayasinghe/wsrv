// SPDX-License-Identifier: Apache-2.0
#include "base/ring_buffer.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <random>
#include <vector>

namespace {

using wsrv::RingBuffer;

std::vector<std::byte> bytes_of(std::uint8_t first, std::size_t count) {
    std::vector<std::byte> out(count);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = static_cast<std::byte>(static_cast<std::uint8_t>(first + i));
    }
    return out;
}

TEST(RingBuffer, StartsEmpty) {
    const RingBuffer ring{8};
    EXPECT_TRUE(ring.empty());
    EXPECT_FALSE(ring.full());
    EXPECT_EQ(ring.capacity(), 8U);
    EXPECT_EQ(ring.size(), 0U);
}

TEST(RingBuffer, WriteAcceptsOnlyWhatFitsAndReportsTheCount) {
    RingBuffer ring{4};
    EXPECT_EQ(ring.write(bytes_of(1, 3)), 3U);
    EXPECT_EQ(ring.write(bytes_of(10, 3)), 1U);  // only one slot left
    EXPECT_TRUE(ring.full());
    EXPECT_EQ(ring.write(bytes_of(20, 1)), 0U);
}

TEST(RingBuffer, ReadReturnsBytesInWriteOrder) {
    RingBuffer ring{8};
    ASSERT_EQ(ring.write(bytes_of(1, 5)), 5U);
    std::vector<std::byte> out(3);
    EXPECT_EQ(ring.read(out), 3U);
    EXPECT_EQ(out, bytes_of(1, 3));
    EXPECT_EQ(ring.size(), 2U);
}

TEST(RingBuffer, WrapAroundPreservesOrder) {
    RingBuffer ring{4};
    ASSERT_EQ(ring.write(bytes_of(1, 4)), 4U);
    ring.consume(3);
    ASSERT_EQ(ring.write(bytes_of(5, 3)), 3U);  // wraps: slots 0,1,2 reused
    std::vector<std::byte> out(4);
    EXPECT_EQ(ring.read(out), 4U);
    EXPECT_EQ(out,
              (std::vector<std::byte>{std::byte{4}, std::byte{5}, std::byte{6}, std::byte{7}}));
}

TEST(RingBuffer, ContiguousFrontNeverExceedsSizeOrTheWrapPoint) {
    RingBuffer ring{4};
    ASSERT_EQ(ring.write(bytes_of(1, 4)), 4U);
    ring.consume(3);
    ASSERT_EQ(ring.write(bytes_of(5, 3)), 3U);
    // data is 4,5,6,7 but physically 4 | 5,6,7 wraps after one byte
    EXPECT_EQ(ring.contiguous_front().size(), 1U);
    ring.consume(1);
    EXPECT_EQ(ring.contiguous_front().size(), 3U);
}

TEST(RingBuffer, ZeroLengthOperationsAreNoOps) {
    RingBuffer ring{4};
    EXPECT_EQ(ring.write({}), 0U);
    std::vector<std::byte> none;
    EXPECT_EQ(ring.read(none), 0U);
    ring.consume(0);
    EXPECT_TRUE(ring.empty());
}

TEST(RingBuffer, ZeroCapacityAcceptsNothing) {
    RingBuffer ring{0};
    EXPECT_EQ(ring.write(bytes_of(1, 2)), 0U);
    EXPECT_TRUE(ring.empty());
    EXPECT_TRUE(ring.full());
}

TEST(RingBuffer, RandomOperationsMatchADequeModel) {
    std::mt19937 rng{12345};
    RingBuffer ring{37};
    std::deque<std::byte> model;
    std::uint8_t next = 0;
    for (int step = 0; step < 20000; ++step) {
        if (rng() % 2 == 0) {
            const std::size_t want = rng() % 20;
            std::vector<std::byte> chunk(want);
            for (auto& b : chunk) {
                b = static_cast<std::byte>(next++);
            }
            const std::size_t accepted = ring.write(chunk);
            ASSERT_EQ(accepted, std::min(want, 37 - model.size()));
            model.insert(
                model.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(accepted));
        } else {
            std::vector<std::byte> out(rng() % 20);
            const std::size_t got = ring.read(out);
            ASSERT_EQ(got, std::min(out.size(), model.size()));
            for (std::size_t i = 0; i < got; ++i) {
                ASSERT_EQ(out[i], model.front());
                model.pop_front();
            }
        }
        ASSERT_EQ(ring.size(), model.size());
        ASSERT_LE(ring.contiguous_front().size(), ring.size());
    }
}

}  // namespace
