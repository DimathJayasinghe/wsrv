// SPDX-License-Identifier: Apache-2.0
// The descriptor budget check (ADR-0022, SECURITY_DESIGN T4): a server that cannot hold its
// connection cap refuses to start, instead of failing with EMFILE under load. The kernel limit is
// pretended by the fake, so no test touches the real process limit.
#include "net/fd_budget.hpp"

#include "support/fake_syscalls.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdint>
#include <limits>

namespace {

using wsrv::ErrorCode;
using wsrv::FdBudget;
using wsrv::test::Call;
using wsrv::test::FakeSyscalls;

constexpr std::uint64_t kUnlimited = std::numeric_limits<std::uint64_t>::max();

TEST(FdBudget, AnAlreadySufficientSoftLimitIsLeftAlone) {
    FakeSyscalls sys;
    sys.pretend_fd_limits(4096, 8192);
    const auto budget = wsrv::ensure_fd_budget(sys, 1000);
    ASSERT_TRUE(budget.has_value());
    EXPECT_EQ(budget->soft, 4096U);
    EXPECT_FALSE(budget->raised);
    EXPECT_EQ(sys.calls(Call::SetFdSoftLimit), 0U) << "no privilege is used that is not needed";
}

TEST(FdBudget, ALowSoftLimitIsRaisedToWhatIsNeededAndNotToTheHardLimit) {
    FakeSyscalls sys;
    sys.pretend_fd_limits(1024, 1'048'576);
    const auto budget = wsrv::ensure_fd_budget(sys, 10'064);
    ASSERT_TRUE(budget.has_value());
    EXPECT_TRUE(budget->raised);
    EXPECT_EQ(budget->soft, 10'064U);
    EXPECT_EQ(sys.pretended_fd_limits().soft, 10'064U);
    EXPECT_EQ(sys.pretended_fd_limits().hard, 1'048'576U) << "the hard limit is never touched";
}

TEST(FdBudget, AHardLimitBelowTheNeedIsAClearRefusal) {
    FakeSyscalls sys;
    sys.pretend_fd_limits(1024, 4096);
    const auto budget = wsrv::ensure_fd_budget(sys, 10'064);
    ASSERT_FALSE(budget.has_value());
    EXPECT_EQ(budget.error().code, ErrorCode::FdLimit);
    EXPECT_EQ(sys.calls(Call::SetFdSoftLimit), 0U) << "raising cannot help: do not try";
    EXPECT_EQ(sys.pretended_fd_limits().soft, 1024U);
}

TEST(FdBudget, ARefusedRaiseIsARefusalNotASilentContinue) {
    FakeSyscalls sys;
    sys.pretend_fd_limits(1024, 1'048'576);
    sys.fail(Call::SetFdSoftLimit, EPERM);
    const auto budget = wsrv::ensure_fd_budget(sys, 10'064);
    ASSERT_FALSE(budget.has_value());
    EXPECT_EQ(budget.error().code, ErrorCode::FdLimit);
}

TEST(FdBudget, AnUnreadableLimitIsAnIoErrorNotAGuess) {
    FakeSyscalls sys;
    sys.fail(Call::GetFdLimits, EFAULT);
    const auto budget = wsrv::ensure_fd_budget(sys, 100);
    ASSERT_FALSE(budget.has_value());
    EXPECT_EQ(budget.error().code, ErrorCode::Io);
}

TEST(FdBudget, UnlimitedMeansEnough) {
    FakeSyscalls sys;
    sys.pretend_fd_limits(kUnlimited, kUnlimited);
    const auto budget = wsrv::ensure_fd_budget(sys, 1'000'000);
    ASSERT_TRUE(budget.has_value());
    EXPECT_FALSE(budget->raised);
}

TEST(FdBudget, TheLimitsOfTheRealKernelAreReadable) {
    wsrv::LinuxSyscalls sys;
    wsrv::FdLimits limits;
    const wsrv::SysResult result = sys.get_fd_limits(limits);
    ASSERT_EQ(result.err, 0);
    EXPECT_GT(limits.soft, 0U);
    EXPECT_LE(limits.soft, limits.hard);
}

TEST(FdBudget, TheRealSetrlimitWrapperKeepsTheHardLimitAndRefusesASoftAboveIt) {
    wsrv::LinuxSyscalls sys;
    wsrv::FdLimits before;
    ASSERT_EQ(sys.get_fd_limits(before).err, 0);
    EXPECT_EQ(sys.set_fd_soft_limit(before.soft).err, 0);  // same value: a no-op for the kernel
    wsrv::FdLimits after;
    ASSERT_EQ(sys.get_fd_limits(after).err, 0);
    EXPECT_EQ(after.soft, before.soft);
    EXPECT_EQ(after.hard, before.hard) << "only the soft limit is ever changed";
    if (before.hard != kUnlimited) {
        EXPECT_EQ(sys.set_fd_soft_limit(before.hard + 1).err, EINVAL) << "soft may not pass hard";
    }
}

}  // namespace
