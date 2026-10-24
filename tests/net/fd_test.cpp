// SPDX-License-Identifier: Apache-2.0
#include "net/fd.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace {

using wsrv::Fd;

struct Pipe {
    int read_end = -1;
    int write_end = -1;
    Pipe() {
        int fds[2];
        EXPECT_EQ(::pipe2(fds, O_CLOEXEC), 0);
        read_end = fds[0];
        write_end = fds[1];
    }
};

bool is_open(int fd) {
    return ::fcntl(fd, F_GETFD) != -1 || errno != EBADF;
}

TEST(Fd, ADefaultFdOwnsNothing) {
    const Fd fd;
    EXPECT_FALSE(fd.valid());
    EXPECT_EQ(fd.get(), -1);
}

TEST(Fd, ClosesTheDescriptorExactlyOnceWhenDestroyed) {
    const Pipe pipe;
    {
        const Fd owner{pipe.write_end};
        EXPECT_TRUE(is_open(pipe.write_end));
    }
    EXPECT_FALSE(is_open(pipe.write_end));
    EXPECT_EQ(::read(pipe.read_end, &errno, 1), 0);  // the other end sees EOF: it really closed
    ::close(pipe.read_end);
}

TEST(Fd, MovingTransfersOwnershipAndEmptiesTheSource) {
    const Pipe pipe;
    Fd a{pipe.read_end};
    Fd b{std::move(a)};
    EXPECT_FALSE(a.valid());  // NOLINT(bugprone-use-after-move): a moved-from Fd is specified empty
    EXPECT_EQ(b.get(), pipe.read_end);
    Fd c;
    c = std::move(b);
    EXPECT_FALSE(b.valid());  // NOLINT(bugprone-use-after-move)
    EXPECT_EQ(c.get(), pipe.read_end);
    ::close(pipe.write_end);
}

TEST(Fd, MoveAssignmentClosesWhatItAlreadyOwned) {
    const Pipe first;
    const Pipe second;
    Fd a{first.read_end};
    Fd b{second.read_end};
    a = std::move(b);
    EXPECT_FALSE(is_open(first.read_end));
    EXPECT_TRUE(is_open(second.read_end));
    ::close(first.write_end);
    ::close(second.write_end);
}

TEST(Fd, SelfMoveAssignmentKeepsTheDescriptor) {
    const Pipe pipe;
    Fd a{pipe.read_end};
    Fd& alias = a;
    a = std::move(alias);
    EXPECT_TRUE(a.valid());
    EXPECT_TRUE(is_open(pipe.read_end));
    ::close(pipe.write_end);
}

TEST(Fd, ReleaseGivesUpOwnershipWithoutClosing) {
    const Pipe pipe;
    Fd owner{pipe.write_end};
    EXPECT_EQ(owner.release(), pipe.write_end);
    EXPECT_FALSE(owner.valid());
    EXPECT_TRUE(is_open(pipe.write_end));
    ::close(pipe.write_end);
    ::close(pipe.read_end);
}

TEST(Fd, ResetClosesTheOldDescriptorAndTakesTheNewOne) {
    const Pipe first;
    const Pipe second;
    Fd owner{first.read_end};
    owner.reset(second.read_end);
    EXPECT_FALSE(is_open(first.read_end));
    EXPECT_EQ(owner.get(), second.read_end);
    owner.reset();
    EXPECT_FALSE(owner.valid());
    EXPECT_FALSE(is_open(second.read_end));
    ::close(first.write_end);
    ::close(second.write_end);
}

TEST(Fd, ResettingToTheSameDescriptorDoesNotCloseIt) {
    const Pipe pipe;
    Fd owner{pipe.read_end};
    owner.reset(pipe.read_end);
    EXPECT_TRUE(is_open(pipe.read_end));
    ::close(pipe.write_end);
}

}  // namespace
