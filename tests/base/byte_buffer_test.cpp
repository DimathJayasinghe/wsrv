// SPDX-License-Identifier: Apache-2.0
#include "base/byte_buffer.hpp"

#include "base/bytes.hpp"

#include <gtest/gtest.h>

#include <string_view>

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
    // A peer must not be able to make us allocate max_size by merely connecting.
    ByteBuffer buffer{std::size_t{1} << 40};
    ASSERT_TRUE(buffer.append(wsrv::as_bytes("x")).has_value());
    EXPECT_EQ(buffer.size(), 1U);
}

TEST(ByteBufferDeathTest, ConsumingMoreThanHeldAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    ByteBuffer buffer{4};
    EXPECT_DEATH({ buffer.consume(1); }, "WSRV_ASSERT failed");
}

}  // namespace
