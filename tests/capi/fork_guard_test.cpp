// SPDX-License-Identifier: Apache-2.0
#include "capi/fork_guard.hpp"

#include <gtest/gtest.h>

#include <sys/wait.h>
#include <unistd.h>

namespace wsrv {
namespace {

/// Run `body` in a forked child and return its exit code (the child must not use gtest).
template <typename Body>
int in_child(Body body) {
    const pid_t child = ::fork();
    if (child == 0) {
        ::alarm(10);  // a hang ends the child by SIGALRM, which the parent reports as a failure
        ::_exit(body() ? 0 : 1);
    }
    int status = 0;
    if (child < 0 || ::waitpid(child, &status, 0) != child || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

TEST(ForkGuard, AGuardIsNotForkedInTheProcessThatMadeIt) {
    const ForkGuard guard;
    EXPECT_FALSE(guard.forked(::getpid()));
}

TEST(ForkGuard, AChildOfTheProcessSeesTheGuardAsForked) {
    const ForkGuard guard;
    const pid_t owner = ::getpid();
    EXPECT_EQ(in_child([&] { return guard.forked(owner); }), 0)
        << "the page marked MADV_WIPEONFORK must read as wiped in the child";
}

TEST(ForkGuard, AChildOfAChildSeesTheGuardAsForkedToo) {
    const ForkGuard guard;
    const pid_t owner = ::getpid();
    EXPECT_EQ(in_child([&] { return in_child([&] { return guard.forked(owner); }) == 0; }), 0);
}

TEST(ForkGuard, TheGetpidFallbackAnswersTheSameInTheOwnerAndInAChild) {
    const ForkGuard guard{false};  // what runs on a kernel without MADV_WIPEONFORK
    const pid_t owner = ::getpid();
    EXPECT_FALSE(guard.forked(owner));
    EXPECT_EQ(in_child([&] { return guard.forked(owner); }), 0);
}

}  // namespace
}  // namespace wsrv
