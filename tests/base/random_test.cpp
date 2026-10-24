// SPDX-License-Identifier: Apache-2.0
#include "base/random.hpp"

#include "support/fakes.hpp"

#include <gtest/gtest.h>

#include <array>

TEST(FakeRandom, ProducesAnIncrementingSequenceAcrossCalls) {
    wsrv::test::FakeRandom random{10};
    std::array<std::byte, 3> a{};
    std::array<std::byte, 2> b{};
    ASSERT_TRUE(random.fill(a).has_value());
    ASSERT_TRUE(random.fill(b).has_value());
    EXPECT_EQ(a[0], std::byte{10});
    EXPECT_EQ(a[2], std::byte{12});
    EXPECT_EQ(b[0], std::byte{13});
}

TEST(FakeRandom, InjectedFailureHitsExactlyOneCall) {
    wsrv::test::FakeRandom random;
    std::array<std::byte, 1> out{};
    random.fail_next();
    EXPECT_EQ(random.fill(out).error().code, wsrv::ErrorCode::RandomUnavailable);
    EXPECT_TRUE(random.fill(out).has_value());
}
