// SPDX-License-Identifier: Apache-2.0
#include "base/checked.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <limits>

namespace {

using wsrv::checked_add;
using wsrv::checked_mul;
using wsrv::ErrorCode;

TEST(Checked, AddInRangeReturnsTheSum) {
    EXPECT_EQ(checked_add<std::uint8_t>(250, 5).value(), 255);
    EXPECT_EQ(checked_add<std::uint32_t>(0, 0).value(), 0U);
}

TEST(Checked, AddOverflowIsAnErrorForEveryWidth) {
    EXPECT_EQ(checked_add<std::uint8_t>(250, 6).error().code, ErrorCode::Overflow);
    EXPECT_EQ(checked_add<std::uint16_t>(65535, 1).error().code, ErrorCode::Overflow);
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(checked_add<std::uint64_t>(kMax, 1).error().code, ErrorCode::Overflow);
    EXPECT_EQ(checked_add<std::uint64_t>(1, kMax).error().code, ErrorCode::Overflow);
    EXPECT_EQ(checked_add<std::uint64_t>(kMax, 0).value(), kMax);
}

TEST(Checked, MulInRangeReturnsTheProduct) {
    EXPECT_EQ(checked_mul<std::uint32_t>(65535, 65537).value(), 4294967295U);
    EXPECT_EQ(checked_mul<std::uint8_t>(15, 17).value(), 255);
}

TEST(Checked, MulOverflowIsAnError) {
    EXPECT_EQ(checked_mul<std::uint32_t>(65536, 65536).error().code, ErrorCode::Overflow);
    EXPECT_EQ(checked_mul<std::uint16_t>(256, 256).error().code, ErrorCode::Overflow);
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(checked_mul<std::uint64_t>(kMax, 2).error().code, ErrorCode::Overflow);
}

TEST(Checked, OperandsAtTheMaximumBehaveExactly) {
    constexpr auto kMax64 = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(checked_mul<std::uint64_t>(kMax64, 1).value(), kMax64);
    EXPECT_EQ(checked_mul<std::uint64_t>(1, kMax64).value(), kMax64);
    EXPECT_EQ(checked_add<std::uint8_t>(0, 255).value(), 255);
    EXPECT_EQ(checked_add<std::uint8_t>(255, 255).error().code, ErrorCode::Overflow);
    EXPECT_EQ(checked_mul<std::uint8_t>(255, 255).error().code, ErrorCode::Overflow);
}

TEST(Checked, MulByZeroIsZeroEvenAtTheMaximum) {
    constexpr auto kMax = std::numeric_limits<std::uint64_t>::max();
    EXPECT_EQ(checked_mul<std::uint64_t>(kMax, 0).value(), 0U);
    EXPECT_EQ(checked_mul<std::uint64_t>(0, kMax).value(), 0U);
}

TEST(Checked, WorksInConstantExpressions) {
    static_assert(checked_add<std::uint8_t>(1, 2).value() == 3);
    static_assert(!checked_mul<std::uint8_t>(16, 16).has_value());
    SUCCEED();
}

}  // namespace
