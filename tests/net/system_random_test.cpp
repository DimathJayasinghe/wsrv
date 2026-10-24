// SPDX-License-Identifier: Apache-2.0
#include "net/system_random.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <vector>

namespace {

TEST(SystemRandom, FillsRequestedBytes) {
    wsrv::SystemRandom random;
    std::array<std::byte, 64> out{};
    ASSERT_TRUE(random.fill(out).has_value());
    EXPECT_FALSE(
        std::all_of(out.begin(), out.end(), [](std::byte b) { return b == std::byte{0}; }));
}

TEST(SystemRandom, TwoFillsDiffer) {
    // Equal 64-byte outputs would happen with probability 2^-512.
    wsrv::SystemRandom random;
    std::array<std::byte, 64> a{};
    std::array<std::byte, 64> b{};
    ASSERT_TRUE(random.fill(a).has_value());
    ASSERT_TRUE(random.fill(b).has_value());
    EXPECT_NE(a, b);
}

TEST(SystemRandom, ZeroLengthFillSucceeds) {
    wsrv::SystemRandom random;
    EXPECT_TRUE(random.fill({}).has_value());
}

TEST(SystemRandom, LargeFillsAreCompleteDespiteShortReads) {
    wsrv::SystemRandom random;
    std::vector<std::byte> out(1 << 20);  // 1 MiB: exercises a large request (short reads are
                                          // legal, though the kernel rarely produces them)
    ASSERT_TRUE(random.fill(out).has_value());
    // The last bytes must have been written too (a short read left unhandled would leave zeros).
    EXPECT_FALSE(
        std::all_of(out.end() - 4096, out.end(), [](std::byte b) { return b == std::byte{0}; }));
}

TEST(SystemRandom, UsableThroughTheInterface) {
    wsrv::SystemRandom concrete;
    wsrv::IRandom& random = concrete;
    std::array<std::byte, 8> out{};
    EXPECT_TRUE(random.fill(out).has_value());
}

}  // namespace
