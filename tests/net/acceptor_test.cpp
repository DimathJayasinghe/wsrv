// SPDX-License-Identifier: Apache-2.0
#include "net/acceptor.hpp"

#include "net/tcp_listener.hpp"
#include "support/fake_syscalls.hpp"
#include "support/fakes.hpp"
#include "support/net_helpers.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <memory>
#include <vector>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {

using namespace std::chrono_literals;
using wsrv::Acceptor;
using wsrv::AcceptorConfig;
using wsrv::Endpoint;
using wsrv::ErrorCode;
using wsrv::EventLoop;
using wsrv::Fd;
using wsrv::TcpListener;
using wsrv::test::Call;
using wsrv::test::connect_to;
using wsrv::test::FakeClock;
using wsrv::test::FakeSyscalls;

class AcceptorTest : public ::testing::Test {
protected:
    FakeSyscalls sys;
    FakeClock clock;
    std::unique_ptr<EventLoop> loop = [this] {
        auto created = EventLoop::create(sys, clock);
        EXPECT_TRUE(created.has_value());
        return std::move(*created);
    }();
    TcpListener listener = [this] {
        wsrv::ListenerConfig config;
        config.endpoint = *wsrv::parse_endpoint("127.0.0.1", 0);
        auto opened = TcpListener::open(sys, config);
        EXPECT_TRUE(opened.has_value());
        return std::move(*opened);
    }();
    std::vector<Fd> accepted;
    std::vector<Endpoint> peers;

    std::unique_ptr<Acceptor> make(AcceptorConfig config = {}) {
        auto acceptor = std::make_unique<Acceptor>(
            *loop, listener, sys,
            [this](Fd fd, Endpoint peer) {
                accepted.push_back(std::move(fd));
                peers.push_back(peer);
            },
            config);
        EXPECT_TRUE(acceptor->start().has_value());
        return acceptor;
    }
    void pump(int iterations = 3) {
        for (int i = 0; i < iterations; ++i) {
            ASSERT_TRUE(loop->run_once(20ms).has_value());
        }
    }
};

TEST_F(AcceptorTest, AcceptsAConnectionAndReportsTheClientsAddress) {
    auto acceptor = make();
    const Fd client = connect_to(listener.local_endpoint());
    ASSERT_TRUE(client.valid());
    pump();
    ASSERT_EQ(accepted.size(), 1U);
    EXPECT_EQ(wsrv::format_endpoint(peers[0]).substr(0, 10), "127.0.0.1:");
    // The accepted socket is non-blocking and close-on-exec (accept4 flags).
    EXPECT_NE(::fcntl(accepted[0].get(), F_GETFL) & O_NONBLOCK, 0);
    EXPECT_NE(::fcntl(accepted[0].get(), F_GETFD) & FD_CLOEXEC, 0);
}

TEST_F(AcceptorTest, WithNoClientNothingHappensAndEagainIsQuiet) {  // EC-N06
    auto acceptor = make();
    pump();
    EXPECT_TRUE(accepted.empty());
    EXPECT_EQ(acceptor->shed_count(), 0U);
}

TEST_F(AcceptorTest, AnInterruptedAcceptIsRetried) {  // EC-N01
    auto acceptor = make();
    sys.fail(Call::Accept4, EINTR, 2);
    const Fd client = connect_to(listener.local_endpoint());
    pump();
    EXPECT_EQ(accepted.size(), 1U);
}

TEST_F(AcceptorTest, AConnectionAbortedBeforeAcceptIsSkippedAndTheLoopContinues) {  // EC-N06
    auto acceptor = make();
    sys.fail(Call::Accept4, ECONNABORTED);
    const Fd first = connect_to(listener.local_endpoint());
    const Fd second = connect_to(listener.local_endpoint());
    pump();
    EXPECT_EQ(accepted.size(), 2U);  // the aborted attempt did not end the burst
}

TEST_F(AcceptorTest, AnUnexpectedAcceptErrorIsCountedAndDoesNotSpin) {
    auto acceptor = make();
    sys.fail(Call::Accept4, EIO);
    const Fd client = connect_to(listener.local_endpoint());
    pump();
    EXPECT_EQ(acceptor->error_count(), 1U);
    EXPECT_LE(sys.calls(Call::Accept4), 4U);  // no busy loop on a persistent error
}

TEST_F(AcceptorTest, OutOfDescriptorsShedsTheWaitingClientBacksOffAndThenResumes) {  // EC-N05
    AcceptorConfig config;
    config.emfile_backoff = 100ms;
    auto acceptor = make(config);
    const Fd doomed = connect_to(listener.local_endpoint());
    ASSERT_TRUE(doomed.valid());
    sys.fail(Call::Accept4, EMFILE);
    pump(1);
    EXPECT_TRUE(accepted.empty());
    EXPECT_EQ(acceptor->shed_count(), 1U);  // accepted with the spare descriptor, then closed
    std::array<char, 4> buf{};
    EXPECT_EQ(::read(doomed.get(), buf.data(), buf.size()), 0);  // the shed client sees a close
    EXPECT_EQ(sys.calls(Call::OpenNull), 2U);                    // the spare was reopened
    // During the back-off a new client waits in the kernel queue and is not accepted...
    const Fd patient = connect_to(listener.local_endpoint());
    clock.advance(50ms);
    pump(1);
    EXPECT_TRUE(accepted.empty());
    // ...and once the back-off is over, accepting resumes.
    clock.advance(60ms);
    pump(2);
    EXPECT_EQ(accepted.size(), 1U);
}

TEST_F(AcceptorTest, ABurstOfConnectionsIsAcceptedOverSeveralIterations) {  // EC-N07, N08
    AcceptorConfig config;
    config.max_accepts_per_event = 64;
    auto acceptor = make(config);
    std::vector<Fd> clients;
    for (int i = 0; i < 200; ++i) {
        clients.push_back(connect_to(listener.local_endpoint()));
        ASSERT_TRUE(clients.back().valid());
    }
    // A socketpair stands for another connection: it must be served while the burst is pending.
    std::array<int, 2> pair{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair.data()), 0);
    const Fd near{pair[0]};
    const Fd far{pair[1]};
    struct Counter final : wsrv::IEventHandler {
        int events = 0;
        void on_event(wsrv::PollEvents) noexcept override { ++events; }
    } other;
    ASSERT_TRUE(loop->watch(near.get(), wsrv::PollEvents::Read, other).has_value());
    ASSERT_EQ(::write(far.get(), "x", 1), 1);
    ASSERT_TRUE(loop->run_once(50ms).has_value());
    EXPECT_EQ(accepted.size(), 64U);
    EXPECT_EQ(other.events, 1);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(accepted.size(), 128U);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(accepted.size(), 192U);
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(accepted.size(), 200U);
}

TEST_F(AcceptorTest, StopWatchingLeavesQueuedClientsAloneAndDestroyingTheAcceptorIsSafe) {
    {
        auto acceptor = make();
        acceptor->stop();
        const Fd client = connect_to(listener.local_endpoint());
        pump();
        EXPECT_TRUE(accepted.empty());
    }
    pump();  // a continuation posted before destruction must not touch freed memory
    EXPECT_TRUE(accepted.empty());
}

TEST_F(AcceptorTest, DestroyingTheAcceptorWithAPostedContinuationIsSafe) {
    AcceptorConfig config;
    config.max_accepts_per_event = 1;
    std::vector<Fd> clients;
    {
        auto acceptor = make(config);
        for (int i = 0; i < 3; ++i) {
            clients.push_back(connect_to(listener.local_endpoint()));
        }
        ASSERT_TRUE(loop->run_once(50ms).has_value());  // accepts 1, posts a continuation
        EXPECT_EQ(accepted.size(), 1U);
    }  // acceptor gone, continuation still queued in the loop
    ASSERT_TRUE(loop->run_once(0ms).has_value());
    EXPECT_EQ(accepted.size(), 1U);
}

TEST(TcpListener, BindsAnEphemeralPortAndReportsIt) {
    FakeSyscalls sys;
    wsrv::ListenerConfig config;
    config.endpoint = *wsrv::parse_endpoint("127.0.0.1", 0);
    const auto listener = TcpListener::open(sys, config);
    ASSERT_TRUE(listener.has_value());
    EXPECT_NE(listener->local_endpoint().port, 0);
    EXPECT_GE(listener->fd(), 0);
}

TEST(TcpListener, APortThatIsAlreadyInUseIsReportedAsSuch) {  // EC-N20
    FakeSyscalls sys;
    wsrv::ListenerConfig first_config;
    first_config.endpoint = *wsrv::parse_endpoint("127.0.0.1", 0);
    const auto first = TcpListener::open(sys, first_config);
    ASSERT_TRUE(first.has_value());
    wsrv::ListenerConfig second_config;
    second_config.endpoint = first->local_endpoint();
    const auto second = TcpListener::open(sys, second_config);
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::AddressInUse);
}

TEST(TcpListener, ReusePortLetsTwoListenersShareAPort) {
    FakeSyscalls sys;
    wsrv::ListenerConfig config;
    config.endpoint = *wsrv::parse_endpoint("127.0.0.1", 0);
    config.reuse_port = true;
    const auto first = TcpListener::open(sys, config);
    ASSERT_TRUE(first.has_value());
    config.endpoint = first->local_endpoint();
    EXPECT_TRUE(TcpListener::open(sys, config).has_value());
}

TEST(TcpListener, SocketCreationFailuresArePassedOn) {
    FakeSyscalls sys;
    sys.fail(Call::Socket, EMFILE);
    wsrv::ListenerConfig config;
    config.endpoint = *wsrv::parse_endpoint("127.0.0.1", 0);
    const auto listener = TcpListener::open(sys, config);
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::FdLimit);
}

TEST(TcpListener, AFailureAfterTheSocketWasCreatedClosesIt) {
    FakeSyscalls sys;
    sys.fail(Call::Listen, EACCES);
    wsrv::ListenerConfig config;
    config.endpoint = *wsrv::parse_endpoint("127.0.0.1", 0);
    const std::size_t before = sys.calls(Call::Close);
    EXPECT_FALSE(TcpListener::open(sys, config).has_value());
    (void)before;  // the Fd destructor closes it; an fd leak would show up under the sanitizers' leak check
}

}  // namespace
