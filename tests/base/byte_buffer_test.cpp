// SPDX-License-Identifier: Apache-2.0
#include "base/byte_buffer.hpp"

#include "base/bytes.hpp"
#include "support/sanitized.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <random>
#include <string>
#include <string_view>
#include <vector>

namespace {

using wsrv::ByteBuffer;
using wsrv::ErrorCode;

TEST(ByteBuffer, StartsEmptyAndRemembersItsBound) {
    const ByteBuffer buffer{16};
    EXPECT_EQ(buffer.size(), 0U);
    EXPECT_EQ(buffer.max_size(), 16U);
    EXPECT_TRUE(buffer.view().empty());
}

TEST(ByteBuffer, AppendWithinTheBoundKeepsDataInOrder) {
    ByteBuffer buffer{16};
    ASSERT_TRUE(buffer.append(wsrv::as_bytes("abc")).has_value());
    ASSERT_TRUE(buffer.append(wsrv::as_bytes("def")).has_value());
    EXPECT_EQ(wsrv::as_chars(buffer.view()), "abcdef");
}

TEST(ByteBuffer, AppendPastTheBoundFailsAndLeavesTheBufferUnchanged) {
    ByteBuffer buffer{4};
    ASSERT_TRUE(buffer.append(wsrv::as_bytes("abc")).has_value());
    const auto result = buffer.append(wsrv::as_bytes("de"));
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::BufferFull);
    EXPECT_EQ(wsrv::as_chars(buffer.view()), "abc");
}

TEST(ByteBuffer, FillingToExactlyTheBoundIsAllowed) {
    ByteBuffer buffer{3};
    EXPECT_TRUE(buffer.append(wsrv::as_bytes("abc")).has_value());
    EXPECT_FALSE(buffer.append(wsrv::as_bytes("d")).has_value());
}

TEST(ByteBuffer, ZeroBoundRefusesDataButAcceptsNothing) {
    ByteBuffer buffer{0};
    EXPECT_TRUE(buffer.append({}).has_value());
    EXPECT_EQ(buffer.append(wsrv::as_bytes("x")).error().code, ErrorCode::BufferFull);
}

TEST(ByteBuffer, ConsumeDropsFromTheFrontAndFreesBoundSpace) {
    ByteBuffer buffer{4};
    ASSERT_TRUE(buffer.append(wsrv::as_bytes("abcd")).has_value());
    buffer.consume(2);
    EXPECT_EQ(wsrv::as_chars(buffer.view()), "cd");
    EXPECT_TRUE(buffer.append(wsrv::as_bytes("ef")).has_value());
    EXPECT_EQ(wsrv::as_chars(buffer.view()), "cdef");
}

TEST(ByteBuffer, ClearEmptiesButKeepsTheBound) {
    ByteBuffer buffer{4};
    ASSERT_TRUE(buffer.append(wsrv::as_bytes("abcd")).has_value());
    buffer.clear();
    EXPECT_EQ(buffer.size(), 0U);
    EXPECT_TRUE(buffer.append(wsrv::as_bytes("wxyz")).has_value());
}

TEST(ByteBuffer, DoesNotReserveTheWholeBoundUpFront) {
    // A peer must not be able to make us allocate max_size just by connecting.
    ByteBuffer buffer{std::size_t{1} << 40};
    ASSERT_TRUE(buffer.append(wsrv::as_bytes("x")).has_value());
    EXPECT_EQ(buffer.size(), 1U);
    EXPECT_LE(buffer.capacity(),
              4096U);  // observable without relying on the allocator refusing 1 TiB
}

TEST(ByteBuffer, MemoryNeverExceedsTheBoundEvenWithConsumedBytesAtTheFront) {
    // Review finding: appending one byte and consuming one byte at a full buffer used to leave dead
    // bytes at the front and grow storage to ~2x max_size.
    constexpr std::size_t kBound = 1000;
    ByteBuffer buffer{kBound};
    const std::string filler(kBound, 'x');
    ASSERT_TRUE(buffer.append(wsrv::as_bytes(filler)).has_value());
    for (int i = 0; i < 5000; ++i) {
        buffer.consume(1);
        ASSERT_TRUE(buffer.append(wsrv::as_bytes("y")).has_value()) << i;
        ASSERT_EQ(buffer.size(), kBound);
        ASSERT_LE(buffer.capacity(), kBound) << i;
    }
}

TEST(ByteBuffer, DrainingAndClearingGiveBackLargeCapacity) {
    ByteBuffer buffer{std::size_t{1} << 20};
    const std::string big(500'000, 'z');
    ASSERT_TRUE(buffer.append(wsrv::as_bytes(big)).has_value());
    EXPECT_GE(buffer.capacity(), big.size());
    buffer.clear();
    EXPECT_LE(buffer.capacity(), 16U * 1024U);  // an idle connection holds almost nothing (ARCH 10)

    ASSERT_TRUE(buffer.append(wsrv::as_bytes(big)).has_value());
    buffer.consume(big.size());  // draining by consume() must release it too
    EXPECT_LE(buffer.capacity(), 16U * 1024U);
}

TEST(ByteBuffer, RandomOperationsMatchADequeModelAndStayWithinTheBound) {
    constexpr std::size_t kBound = 1000;
    std::mt19937 rng{4242};
    ByteBuffer buffer{kBound};
    std::deque<std::byte> model;
    std::uint8_t next = 0;
    for (int step = 0; step < 30'000; ++step) {
        const auto op = rng() % 10;
        if (op < 5) {
            std::vector<std::byte> chunk(rng() % 250);
            for (std::byte& b : chunk) {
                b = static_cast<std::byte>(next++);
            }
            const bool fits = model.size() + chunk.size() <= kBound;
            const auto result = buffer.append(chunk);
            ASSERT_EQ(result.has_value(), fits) << step;
            if (fits) {
                model.insert(model.end(), chunk.begin(), chunk.end());
            }
        } else if (op < 9) {
            const std::size_t n = model.empty() ? 0 : rng() % (model.size() + 1);
            buffer.consume(n);
            model.erase(model.begin(), model.begin() + static_cast<std::ptrdiff_t>(n));
        } else {
            buffer.clear();
            model.clear();
        }
        ASSERT_EQ(buffer.size(), model.size()) << step;
        ASSERT_LE(buffer.capacity(), kBound) << step;
        ASSERT_TRUE(
            std::equal(model.begin(), model.end(), buffer.view().begin(), buffer.view().end()))
            << step;
    }
}

TEST(ByteBuffer, AppendingAViewOfItsOwnContentsIsSafe) {
    // Review finding: compaction shifted the bytes under the caller's span (ASan:
    // memcpy-param-overlap).
    ByteBuffer buffer{64};
    ASSERT_TRUE(buffer.append(wsrv::as_bytes("abcdefgh")).has_value());
    buffer.consume(5);
    ASSERT_TRUE(buffer.append(buffer.view()).has_value());
    EXPECT_EQ(wsrv::as_chars(buffer.view()), "fghfgh");

    ASSERT_TRUE(buffer.append(buffer.view().subspan(2, 3)).has_value());  // a partial self-view
    EXPECT_EQ(wsrv::as_chars(buffer.view()), "fghfghhfg");
}

TEST(ByteBuffer, AllocationFailureIsAResourceExhaustedErrorNotAnException) {
#ifdef WSRV_TEST_SANITIZED
    GTEST_SKIP() << "sanitizer allocators abort instead of throwing bad_alloc; covered by the "
                    "release preset";
#endif
    // The span claims 2^50 bytes but is never read: the allocation fails before any copy happens.
    ByteBuffer buffer{std::size_t{1} << 60};
    const std::byte anchor{0};
    const wsrv::ByteView absurd{&anchor, std::size_t{1} << 50};
    const auto result = buffer.append(absurd);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, ErrorCode::ResourceExhausted);
    EXPECT_EQ(buffer.size(), 0U);  // unchanged
    EXPECT_TRUE(buffer.append(wsrv::as_bytes("still works")).has_value());
}

TEST(ByteBufferDeathTest, ConsumingMoreThanHeldAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ByteBuffer buffer{4};
    EXPECT_DEATH({ buffer.consume(1); }, "WSRV_ASSERT failed");
}

}  // namespace
