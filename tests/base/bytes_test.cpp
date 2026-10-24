// SPDX-License-Identifier: Apache-2.0
#include "base/bytes.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <string_view>

namespace {

using wsrv::ByteView;

constexpr std::array<std::byte, 8> kSequence{std::byte{1},
                                             std::byte{2},
                                             std::byte{3},
                                             std::byte{4},
                                             std::byte{5},
                                             std::byte{6},
                                             std::byte{7},
                                             std::byte{8}};

TEST(Bytes, LoadBigEndianReadsMostSignificantByteFirst) {
    EXPECT_EQ(wsrv::load_be16(kSequence), 0x0102U);
    EXPECT_EQ(wsrv::load_be32(kSequence), 0x01020304U);
    EXPECT_EQ(wsrv::load_be64(kSequence), 0x0102030405060708ULL);
}

TEST(Bytes, LoadIgnoresBytesBeyondTheWidth) {
    EXPECT_EQ(wsrv::load_be16(ByteView{kSequence}.subspan(6)), 0x0708U);
}

TEST(Bytes, StoreThenLoadRoundTrips) {
    std::array<std::byte, 8> buffer{};
    wsrv::store_be16(buffer, 0xBEEF);
    EXPECT_EQ(wsrv::load_be16(buffer), 0xBEEFU);
    wsrv::store_be32(buffer, 0xDEADBEEFU);
    EXPECT_EQ(wsrv::load_be32(buffer), 0xDEADBEEFU);
    wsrv::store_be64(buffer, 0x0123456789ABCDEFULL);
    EXPECT_EQ(wsrv::load_be64(buffer), 0x0123456789ABCDEFULL);
    EXPECT_EQ(buffer[0], std::byte{0x01});
    EXPECT_EQ(buffer[7], std::byte{0xEF});
}

TEST(Bytes, AsBytesAndAsCharsRoundTrip) {
    constexpr std::string_view kText = "hello";
    const ByteView bytes = wsrv::as_bytes(kText);
    ASSERT_EQ(bytes.size(), 5U);
    EXPECT_EQ(bytes[0], std::byte{'h'});
    EXPECT_EQ(wsrv::as_chars(bytes), kText);
}

TEST(Bytes, EmptyViewsAreFine) {
    EXPECT_TRUE(wsrv::as_bytes({}).empty());
    EXPECT_TRUE(wsrv::as_chars({}).empty());
}

TEST(BytesDeathTest, LoadFromTooShortViewAborts) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ (void)wsrv::load_be32(ByteView{kSequence}.first(3)); }, "WSRV_ASSERT failed");
}

}  // namespace
