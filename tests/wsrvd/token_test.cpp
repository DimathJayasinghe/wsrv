// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/token.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace {

using wsrv::ByteView;
using wsrv::wsrvd::constant_time_equal;

ByteView bytes(std::string_view text) {
    return ByteView{reinterpret_cast<const std::byte*>(text.data()), text.size()};  // NOLINT
}

TEST(ConstantTimeEqual, EqualSecretsMatch) {
    EXPECT_TRUE(constant_time_equal(bytes("s3cret-token"), bytes("s3cret-token")));
}

TEST(ConstantTimeEqual, AnyDifferentByteFailsWhereverItIs) {
    EXPECT_FALSE(constant_time_equal(bytes("s3cret-token"), bytes("X3cret-token")));  // first
    EXPECT_FALSE(constant_time_equal(bytes("s3cret-token"), bytes("s3cret-tokeX")));  // last
    EXPECT_FALSE(constant_time_equal(bytes("s3cret-token"), bytes("s3cret-Xoken")));  // middle
}

TEST(ConstantTimeEqual, ADifferentLengthFailsEvenWhenOneIsAPrefixOfTheOther) {
    EXPECT_FALSE(constant_time_equal(bytes("token"), bytes("token-and-more")));
    EXPECT_FALSE(constant_time_equal(bytes("token-and-more"), bytes("token")));
}

TEST(ConstantTimeEqual, EmptyMatchesOnlyEmpty) {
    EXPECT_TRUE(constant_time_equal(ByteView{}, ByteView{}));
    EXPECT_FALSE(constant_time_equal(ByteView{}, bytes("x")));
    EXPECT_FALSE(constant_time_equal(bytes("x"), ByteView{}));
}

TEST(ConstantTimeEqual, TrailingZeroBytesAreNotIgnored) {
    // Padding the shorter input with zeros must not make "a" equal "a\0".
    EXPECT_FALSE(constant_time_equal(bytes("a"), bytes(std::string_view{"a\0", 2})));
}

}  // namespace
