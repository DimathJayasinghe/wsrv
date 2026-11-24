// SPDX-License-Identifier: Apache-2.0
#include "net/plain_transport.hpp"

#include "support/fake_syscalls.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <unistd.h>

namespace {

using wsrv::ErrorCode;
using wsrv::Fd;
using wsrv::PlainTransport;
using wsrv::test::Call;
using wsrv::test::FakeSyscalls;

struct Fixture {
    FakeSyscalls sys;
    Fd peer;
    PlainTransport transport;

    static std::pair<Fd, Fd> pair() {
        std::array<int, 2> fds{};
        EXPECT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, fds.data()),
                  0);
        return {Fd{fds[0]}, Fd{fds[1]}};
    }
    Fixture() : Fixture(pair()) {}

private:
    explicit Fixture(std::pair<Fd, Fd> p)
        : peer{std::move(p.second)}, transport{std::move(p.first), sys} {}
};

wsrv::ByteView bytes_of(const std::string& s) {
    return wsrv::as_bytes(std::string_view{s});
}

TEST(PlainTransport, ReadsWhatThePeerSent) {
    Fixture f;
    ASSERT_EQ(::write(f.peer.get(), "hello", 5), 5);
    std::array<std::byte, 16> buf{};
    const auto n = f.transport.read(buf);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 5U);
    EXPECT_EQ(std::string{wsrv::as_chars(wsrv::ByteView{buf}.first(5))}, "hello");
}

TEST(PlainTransport, ReadWithNothingAvailableIsWouldBlockNotAnEof) {  // EC-N01
    Fixture f;
    std::array<std::byte, 16> buf{};
    const auto n = f.transport.read(buf);
    ASSERT_FALSE(n.has_value());
    EXPECT_EQ(n.error().code, ErrorCode::WouldBlock);
}

TEST(PlainTransport, ReadReturnsZeroOnlyWhenThePeerHasFinished) {  // EC-N04
    Fixture f;
    f.peer.reset();
    std::array<std::byte, 16> buf{};
    const auto n = f.transport.read(buf);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 0U);
}

TEST(PlainTransport, AnInterruptedReadIsRetriedTransparently) {  // EC-N01
    Fixture f;
    ASSERT_EQ(::write(f.peer.get(), "x", 1), 1);
    f.sys.fail(Call::Read, EINTR, 3);
    std::array<std::byte, 4> buf{};
    const auto n = f.transport.read(buf);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 1U);
    EXPECT_EQ(f.sys.calls(Call::Read), 4U);
}

TEST(PlainTransport, AResetConnectionIsConnectionClosed) {
    Fixture f;
    f.sys.fail(Call::Read, ECONNRESET);
    std::array<std::byte, 4> buf{};
    const auto n = f.transport.read(buf);
    ASSERT_FALSE(n.has_value());
    EXPECT_EQ(n.error().code, ErrorCode::ConnectionClosed);
}

TEST(PlainTransport, WritesReachThePeer) {
    Fixture f;
    const auto n = f.transport.write(bytes_of("pong"));
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 4U);
    std::array<char, 8> buf{};
    EXPECT_EQ(::read(f.peer.get(), buf.data(), buf.size()), 4);
}

TEST(PlainTransport, AnEmptyWriteIsANoOp) {
    Fixture f;
    const auto n = f.transport.write({});
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 0U);
    EXPECT_EQ(f.sys.calls(Call::Send), 0U);
}

TEST(PlainTransport, AFullSendBufferGivesAPartialWriteThenWouldBlock) {  // EC-N02
    Fixture f;
    const std::string big(8U << 20, 'x');
    std::size_t sent = 0;
    for (;;) {
        const auto n = f.transport.write(wsrv::as_bytes(std::string_view{big}).subspan(sent));
        if (!n.has_value()) {
            EXPECT_EQ(n.error().code, ErrorCode::WouldBlock);
            break;
        }
        sent += *n;
        ASSERT_LT(sent, big.size()) << "an 8 MiB write cannot fit a socketpair buffer";
    }
    EXPECT_GT(sent, 0U);
}

TEST(PlainTransport, WritingToAClosedPeerIsConnectionClosedAndNotASignal) {  // EC-N03
    Fixture f;
    f.peer.reset();
    const auto n = f.transport.write(bytes_of("late"));
    ASSERT_FALSE(n.has_value());  // and, above all, the test process is still alive
    EXPECT_EQ(n.error().code, ErrorCode::ConnectionClosed);
}

TEST(PlainTransport, ShutdownWriteTellsThePeerWeAreDoneButStillReads) {
    Fixture f;
    f.transport.shutdown_write();
    std::array<char, 4> buf{};
    EXPECT_EQ(::read(f.peer.get(), buf.data(), buf.size()), 0);  // EOF at the peer
    ASSERT_EQ(::write(f.peer.get(), "r", 1), 1);
    std::array<std::byte, 4> in{};
    const auto n = f.transport.read(in);
    ASSERT_TRUE(n.has_value());
    EXPECT_EQ(*n, 1U);
}

TEST(PlainTransport, CloseReleasesTheDescriptorAndIsIdempotent) {
    Fixture f;
    const int fd = f.transport.fd();
    EXPECT_GE(fd, 0);
    f.transport.close();
    EXPECT_EQ(f.transport.fd(), -1);
    f.transport.close();
    std::array<char, 4> buf{};
    EXPECT_EQ(::read(f.peer.get(), buf.data(), buf.size()), 0);  // the peer sees the close
    std::array<std::byte, 4> in{};
    EXPECT_FALSE(
        f.transport.read(in).has_value());  // using a closed transport is an error, not a crash
}

TEST(PlainTransport, ASocketTransportVouchesThatAShortReadMeansTheKernelIsDrained) {
    Fixture f;
    EXPECT_TRUE(f.transport.short_read_means_drained());
    EXPECT_FALSE(static_cast<const wsrv::ITransport&>(f.transport).wants_write());
}

}  // namespace
