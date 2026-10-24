// SPDX-License-Identifier: Apache-2.0
#include "base/version.hpp"

#include <gtest/gtest.h>

#include <string_view>

TEST(Version, ReturnsNonEmptyDottedString) {
    const std::string_view v = wsrv::version();
    EXPECT_FALSE(v.empty());
    EXPECT_NE(v.find('.'), std::string_view::npos);
}

TEST(Version, MatchesProjectVersion) {
    EXPECT_EQ(wsrv::version(), std::string_view{WSRV_VERSION});
}
