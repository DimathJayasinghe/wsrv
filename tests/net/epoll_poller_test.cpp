// SPDX-License-Identifier: Apache-2.0
#include "net/epoll_poller.hpp"

#include "net/fd.hpp"
#include "support/fake_syscalls.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <memory>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

using wsrv::ErrorCode;
using wsrv::Fd;
using wsrv::PollEvent;
using wsrv::PollEvents;
using wsrv::test::Call;
using wsrv::test::FakeSyscalls;

constexpr PollEvents kRead = PollEvents::Read;

/// A connected pair of non-blocking sockets: `near` is registered, `far` is the "client".
struct Pair {
    Fd near;
    Fd far;
    Pair() {
        std::array<int, 2> fds{};
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds.data()),
                  0);
        near.reset(fds[0]);
        far.reset(fds[1]);
    }
    void send(const char* text) const {
        EXPECT_GT(::write(far.get(), text, std::char_traits<char>::length(text)), 0);
    }
    void drain() const {
        std::array<char, 256> buf{};
        while (::read(near.get(), buf.data(), buf.size()) > 0) {
        }
    }
};

std::vector<PollEvent> wait(wsrv::IPoller& poller, int timeout_ms = 0, std::size_t slots = 16) {
    std::vector<PollEvent> events(slots);
    const auto n = poller.wait(timeout_ms, events);
    EXPECT_TRUE(n.has_value());
    events.resize(n.value_or(0));
    return events;
}

class EpollPollerTest : public ::testing::Test {
protected:
    FakeSyscalls sys;
    std::unique_ptr<wsrv::EpollPoller> poller = [this] {
        auto created = wsrv::EpollPoller::create(sys);
        EXPECT_TRUE(created.has_value());
        return std::move(*created);
    }();
};

TEST_F(EpollPollerTest, NothingIsReadyOnAFreshRegistration) {
    const Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), kRead, 1).has_value());
    EXPECT_TRUE(wait(*poller).empty());
}

TEST_F(EpollPollerTest, ReportsReadReadinessWithTheRegisteredToken) {
    const Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), kRead, 0xABCDEF).has_value());
    pair.send("hi");
    const auto events = wait(*poller);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].token, 0xABCDEFU);
    EXPECT_TRUE(wsrv::has(events[0].events, PollEvents::Read));
}

TEST_F(EpollPollerTest, IsEdgeTriggeredSoUnreadDataIsReportedOnlyOnce) {  // ADR-0004, EC-N07
    const Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), kRead, 1).has_value());
    pair.send("abc");
    EXPECT_EQ(wait(*poller).size(), 1U);
    EXPECT_TRUE(wait(*poller).empty());  // the bytes are still unread, yet no new event
    pair.send("def");                    // new arrival -> new edge
    EXPECT_EQ(wait(*poller).size(), 1U);
    pair.drain();
}

TEST_F(EpollPollerTest, WriteInterestReportsWritabilityOnce) {
    const Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), PollEvents::Write, 7).has_value());
    const auto events = wait(*poller);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(wsrv::has(events[0].events, PollEvents::Write));
    EXPECT_TRUE(wait(*poller).empty());
}

TEST_F(EpollPollerTest, APeerThatClosesIsReportedAsHangupAndReadable) {
    Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), kRead, 3).has_value());
    pair.far.reset();
    const auto events = wait(*poller);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(wsrv::has(events[0].events, PollEvents::Hangup));
    EXPECT_TRUE(wsrv::has(events[0].events, PollEvents::Read));  // the EOF is read, not guessed
}

TEST_F(EpollPollerTest,
       HangupInterestSeesAPeerCloseButNotIncomingData) {  // ADR-0018: waiting phase
    Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), PollEvents::Hangup, 4).has_value());
    pair.send("data the owner does not want to read yet");
    EXPECT_TRUE(wait(*poller).empty()) << "data is not reported when Read is not wanted";
    ASSERT_EQ(::shutdown(pair.far.get(), SHUT_WR), 0);  // a FIN: the peer is done sending
    const auto events = wait(*poller);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_TRUE(wsrv::has(events[0].events, PollEvents::Hangup));
    EXPECT_FALSE(wsrv::has(events[0].events, PollEvents::Read));
}

TEST_F(EpollPollerTest, ModifyChangesTheInterestAndRearmsTheEdge) {
    const Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), kRead, 5).has_value());
    EXPECT_TRUE(wait(*poller).empty());
    ASSERT_TRUE(poller->modify(pair.near.get(), PollEvents::Write, 6).has_value());
    const auto events = wait(*poller);
    ASSERT_EQ(events.size(), 1U);
    EXPECT_EQ(events[0].token, 6U);  // the token can change too
    EXPECT_TRUE(wsrv::has(events[0].events, PollEvents::Write));
}

TEST_F(EpollPollerTest, RemoveStopsAllEvents) {
    const Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), kRead, 1).has_value());
    ASSERT_TRUE(poller->remove(pair.near.get()).has_value());
    pair.send("x");
    EXPECT_TRUE(wait(*poller).empty());
}

TEST_F(EpollPollerTest, RegistrationMistakesAreErrorsNotCrashes) {
    const Pair pair;
    ASSERT_TRUE(poller->add(pair.near.get(), kRead, 1).has_value());
    EXPECT_FALSE(poller->add(pair.near.get(), kRead, 2).has_value());  // twice
    EXPECT_FALSE(poller->remove(pair.far.get()).has_value());          // never added
    EXPECT_FALSE(poller->modify(pair.far.get(), kRead, 1).has_value());
    EXPECT_FALSE(poller->add(-1, kRead, 1).has_value());  // not a descriptor
}

TEST_F(EpollPollerTest, DeliversNoMoreEventsThanTheCallerHasRoomForAndKeepsTheRest) {
    std::vector<Pair> pairs(5);
    for (std::size_t i = 0; i < pairs.size(); ++i) {
        ASSERT_TRUE(poller->add(pairs[i].near.get(), kRead, i).has_value());
        pairs[i].send("x");
    }
    EXPECT_EQ(wait(*poller, 0, 2).size(), 2U);
    EXPECT_EQ(wait(*poller, 0, 2).size(), 2U);
    EXPECT_EQ(wait(*poller, 0, 2).size(), 1U);
    EXPECT_TRUE(wait(*poller, 0, 2).empty());
}

TEST_F(EpollPollerTest, WaitHonoursItsTimeout) {
    const auto before = std::chrono::steady_clock::now();
    EXPECT_TRUE(wait(*poller, 30).empty());
    EXPECT_GE(std::chrono::steady_clock::now() - before, std::chrono::milliseconds{20});
    const auto instant = std::chrono::steady_clock::now();
    EXPECT_TRUE(wait(*poller, 0).empty());
    EXPECT_LT(std::chrono::steady_clock::now() - instant, std::chrono::milliseconds{20});
}

TEST_F(EpollPollerTest, AnInterruptedWaitIsZeroEventsNotAnError) {  // EC-N01
    sys.fail(Call::EpollWait, EINTR);
    std::vector<PollEvent> events(4);
    const auto n = poller->wait(50, events);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 0U);
}

TEST_F(EpollPollerTest, OtherWaitFailuresAreReported) {
    sys.fail(Call::EpollWait, EBADF);
    std::vector<PollEvent> events(4);
    const auto n = poller->wait(0, events);
    ASSERT_FALSE(n.has_value());
    EXPECT_EQ(n.error().code, ErrorCode::Io);
}

TEST(EpollPollerCreate, DescriptorExhaustionIsReportedAsAFdLimit) {
    FakeSyscalls sys;
    sys.fail(Call::EpollCreate1, EMFILE);
    const auto created = wsrv::EpollPoller::create(sys);
    ASSERT_FALSE(created.has_value());
    EXPECT_EQ(created.error().code, ErrorCode::FdLimit);
}

}  // namespace
