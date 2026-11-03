// SPDX-License-Identifier: Apache-2.0
#include "base/error.hpp"

#include <gtest/gtest.h>

#include <array>
#include <set>
#include <string_view>

namespace {

using wsrv::Error;
using wsrv::ErrorCategory;
using wsrv::ErrorCode;
using wsrv::Result;

constexpr std::array kAllCodes{ErrorCode::InvalidArgument,
                               ErrorCode::OutOfRange,
                               ErrorCode::Overflow,
                               ErrorCode::BufferFull,
                               ErrorCode::InvalidBase64,
                               ErrorCode::InvalidUtf8,
                               ErrorCode::RandomUnavailable,
                               ErrorCode::ResourceExhausted,
                               ErrorCode::Closing,
                               ErrorCode::Backpressure,
                               ErrorCode::InvalidConfig,
                               ErrorCode::WouldBlock,
                               ErrorCode::ConnectionClosed,
                               ErrorCode::FdLimit,
                               ErrorCode::AddressInUse,
                               ErrorCode::Io,
                               ErrorCode::MessageInProgress,
                               ErrorCode::WrongProcess,
                               ErrorCode::InvalidFrame};

TEST(Error, EveryCodeHasANonEmptyDistinctName) {
    std::set<std::string_view> names;
    for (const ErrorCode code : kAllCodes) {
        const std::string_view name = wsrv::to_string(code);
        EXPECT_FALSE(name.empty());
        names.insert(name);
    }
    EXPECT_EQ(names.size(), kAllCodes.size());
}

TEST(Error, CategoriesMatchTheErrorModel) {
    EXPECT_EQ(wsrv::category(ErrorCode::BufferFull), ErrorCategory::Limit);
    EXPECT_EQ(wsrv::category(ErrorCode::InvalidUtf8), ErrorCategory::Protocol);
    EXPECT_EQ(wsrv::category(ErrorCode::InvalidBase64), ErrorCategory::Protocol);
    EXPECT_EQ(wsrv::category(ErrorCode::Overflow), ErrorCategory::Limit);
    EXPECT_EQ(wsrv::category(ErrorCode::ResourceExhausted), ErrorCategory::Limit);
    EXPECT_EQ(wsrv::category(ErrorCode::RandomUnavailable), ErrorCategory::Internal);
    EXPECT_EQ(wsrv::category(ErrorCode::InvalidArgument), ErrorCategory::Internal);
    EXPECT_EQ(wsrv::category(ErrorCode::Closing), ErrorCategory::Internal);
    EXPECT_EQ(wsrv::category(ErrorCode::Backpressure), ErrorCategory::Limit);
    EXPECT_EQ(wsrv::category(ErrorCode::InvalidConfig), ErrorCategory::Config);
    EXPECT_EQ(wsrv::category(ErrorCode::InvalidFrame), ErrorCategory::Protocol);
}

TEST(Error, FailBuildsAnErrorResultCarryingCodeAndContext) {
    const Result<int> r = wsrv::fail(ErrorCode::Overflow, "length too large");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Overflow);
    EXPECT_EQ(r.error().context, "length too large");
}

TEST(Error, ContextDefaultsToEmpty) {
    const Result<int> r = wsrv::fail(ErrorCode::BufferFull);
    EXPECT_TRUE(r.error().context.empty());
}

TEST(Error, ErrorsCompareByCodeAndContext) {
    EXPECT_EQ((Error{ErrorCode::OutOfRange, "a"}), (Error{ErrorCode::OutOfRange, "a"}));
    EXPECT_NE((Error{ErrorCode::OutOfRange, "a"}), (Error{ErrorCode::OutOfRange, "b"}));
}

}  // namespace
