// SPDX-License-Identifier: Apache-2.0
#include "base/assert.hpp"

#include <gtest/gtest.h>

TEST(AssertDeathTest, FailingAssertAbortsAndNamesTheCondition) {
    GTEST_FLAG_SET(death_test_style, "threadsafe");
    EXPECT_DEATH({ WSRV_ASSERT(1 + 1 == 3); }, "WSRV_ASSERT failed: 1 \\+ 1 == 3");
}

TEST(Assert, PassingAssertDoesNothing) {
    WSRV_ASSERT(1 + 1 == 2);
    SUCCEED();
}
