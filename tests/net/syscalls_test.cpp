// SPDX-License-Identifier: Apache-2.0
#include "net/syscalls.hpp"

#include "net/endpoint.hpp"
#include "support/fake_syscalls.hpp"

#include <gtest/gtest.h>

#include <array>
#include <cerrno>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>

#include <netinet/in.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

using wsrv::ErrorCode;
using wsrv::SysResult;
using wsrv::test::Call;
using wsrv::test::FakeSyscalls;

TEST(LinuxSyscalls, ReportsSuccessAndFailureWithoutTouchingTheGlobalErrno) {
    wsrv::LinuxSyscalls sys;
    std::array<int, 2> pair{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, pair.data()), 0);
    std::array<char, 8> buf{};
    const SysResult empty = sys.read(pair[0], buf.data(), buf.size());
    EXPECT_EQ(empty.err, EAGAIN);  // nothing to read yet: would block
    EXPECT_EQ(empty.value, -1);
    const SysResult sent = sys.send(pair[1], "abc", 3, MSG_NOSIGNAL);
    EXPECT_EQ(sent.err, 0);
    EXPECT_EQ(sent.value, 3);
    const SysResult got = sys.read(pair[0], buf.data(), buf.size());
    EXPECT_EQ(got.value, 3);
    sys.close(pair[1]);
    EXPECT_EQ(sys.read(pair[0], buf.data(), buf.size()).value, 0);  // EOF is 0, not an error
    sys.close(pair[0]);
}

TEST(LinuxSyscalls, SendToAClosedPeerIsEpipeNotASignal) {
    wsrv::LinuxSyscalls sys;
    std::array<int, 2> pair{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()), 0);
    sys.close(pair[1]);
    const SysResult sent = sys.send(pair[0], "x", 1, MSG_NOSIGNAL);
    EXPECT_EQ(sent.err, EPIPE);
    sys.close(pair[0]);
}

TEST(LinuxSyscalls, ListensOnAnEphemeralPortAndReportsIt) {
    wsrv::LinuxSyscalls sys;
    const SysResult fd = sys.socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    ASSERT_EQ(fd.err, 0);
    sockaddr_storage addr{};
    const socklen_t addr_len = wsrv::to_sockaddr(*wsrv::parse_endpoint("127.0.0.1", 0), addr);
    ASSERT_EQ(sys.bind(static_cast<int>(fd.value), wsrv::as_sockaddr(addr), addr_len).err, 0);
    ASSERT_EQ(sys.listen(static_cast<int>(fd.value), 8).err, 0);
    sockaddr_storage bound{};
    socklen_t len = sizeof(bound);
    ASSERT_EQ(sys.getsockname(static_cast<int>(fd.value), wsrv::as_sockaddr(bound), &len).err, 0);
    const auto local = wsrv::from_sockaddr(bound);
    ASSERT_TRUE(local.has_value());
    EXPECT_NE(local->port, 0);
    // Nothing is connecting: a non-blocking accept4 would block.
    EXPECT_EQ(sys.accept4(static_cast<int>(fd.value), nullptr, nullptr, SOCK_NONBLOCK).err, EAGAIN);
    sys.close(static_cast<int>(fd.value));
}

TEST(LinuxSyscalls, EventfdEpollAndDevNullWork) {
    wsrv::LinuxSyscalls sys;
    const SysResult ep = sys.epoll_create1(EPOLL_CLOEXEC);
    const SysResult ev = sys.eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_EQ(ep.err, 0);
    ASSERT_EQ(ev.err, 0);
    epoll_event reg{};
    reg.events = EPOLLIN;
    reg.data.u64 = 42;
    ASSERT_EQ(
        sys.epoll_ctl(static_cast<int>(ep.value), EPOLL_CTL_ADD, static_cast<int>(ev.value), &reg)
            .err,
        0);
    std::array<epoll_event, 4> out{};
    EXPECT_EQ(sys.epoll_wait(static_cast<int>(ep.value), out.data(), 4, 0).value,
              0);  // nothing ready
    const std::uint64_t one = 1;
    ASSERT_EQ(sys.write(static_cast<int>(ev.value), &one, sizeof(one)).value, 8);
    const SysResult ready = sys.epoll_wait(static_cast<int>(ep.value), out.data(), 4, 100);
    ASSERT_EQ(ready.value, 1);
    // epoll_event is packed on x86-64, so data.u64 is misaligned: copy it, do not bind a reference
    const std::uint64_t token = out[0].data.u64;
    EXPECT_EQ(token, 42U);
    const SysResult null_fd = sys.open_null();
    EXPECT_EQ(null_fd.err, 0);
    EXPECT_GE(null_fd.value, 0);
    sys.close(static_cast<int>(null_fd.value));
    sys.close(static_cast<int>(ev.value));
    sys.close(static_cast<int>(ep.value));
}

TEST(Retrying, ReturnsTheValueOfASuccessfulCall) {
    int calls = 0;
    const wsrv::Result<long> r = wsrv::retrying(
        [&] {
            ++calls;
            return SysResult{7, 0};
        },
        "test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 7);
    EXPECT_EQ(calls, 1);
}

TEST(Retrying, RetriesEintrUntilTheCallSucceeds) {
    int calls = 0;
    const wsrv::Result<long> r = wsrv::retrying(
        [&] { return ++calls < 4 ? SysResult{-1, EINTR} : SysResult{5, 0}; }, "test");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, 5);
    EXPECT_EQ(calls, 4);
}

TEST(Retrying, GivesUpAfterTooManyInterruptions) {
    int calls = 0;
    const wsrv::Result<long> r = wsrv::retrying(
        [&] {
            ++calls;
            return SysResult{-1, EINTR};
        },
        "test");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Io);
    EXPECT_EQ(calls, static_cast<int>(wsrv::kMaxEintrRetries));
}

TEST(Retrying, MapsOtherErrorsWithTheCallersContext) {
    const wsrv::Result<long> again =
        wsrv::retrying([] { return SysResult{-1, EAGAIN}; }, "read client");
    ASSERT_FALSE(again.has_value());
    EXPECT_EQ(again.error().code, ErrorCode::WouldBlock);
    EXPECT_EQ(again.error().context, "read client");
    const wsrv::Result<long> pipe =
        wsrv::retrying([] { return SysResult{-1, EPIPE}; }, "write client");
    EXPECT_EQ(pipe.error().code, ErrorCode::ConnectionClosed);
}

TEST(FakeSyscalls, InjectsAFailureOnceThenFallsThroughToTheKernel) {
    FakeSyscalls sys;
    sys.fail(Call::Accept4, EMFILE);
    EXPECT_EQ(sys.accept4(-1, nullptr, nullptr, 0).err,
              EMFILE);  // scripted: the kernel was not asked
    EXPECT_EQ(sys.accept4(-1, nullptr, nullptr, 0).err,
              EBADF);  // queue empty: the real kernel says so
    EXPECT_EQ(sys.calls(Call::Accept4), 2U);
    EXPECT_EQ(sys.pending_script(Call::Accept4), 0U);
}

TEST(FakeSyscalls, InjectsSeveralFailuresAndForcedSuccessValuesInOrder) {
    FakeSyscalls sys;
    sys.fail(Call::Read, EINTR, 2);
    sys.succeed(Call::Read, 0);  // pretend EOF
    EXPECT_EQ(sys.read(-1, nullptr, 0).err, EINTR);
    EXPECT_EQ(sys.read(-1, nullptr, 0).err, EINTR);
    const SysResult eof = sys.read(-1, nullptr, 0);
    EXPECT_EQ(eof.err, 0);
    EXPECT_EQ(eof.value, 0);
}

// ---- the filesystem calls (ADR-0017 section 4) -------------------------------------------------

TEST(LinuxSyscalls, StatPathFillsFileInfoAndLstatDoesNotFollowLinks) {
    namespace fs = std::filesystem;
    wsrv::LinuxSyscalls sys;
    std::string pattern = (fs::temp_directory_path() / "wsrv-sys-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    const std::string file = pattern + "/file";
    const std::string link = pattern + "/link";
    std::ofstream{file} << "x";
    ASSERT_EQ(::symlink(file.c_str(), link.c_str()), 0);

    wsrv::FileInfo info;
    ASSERT_EQ(sys.stat_path(file.c_str(), false, info).err, 0);
    EXPECT_EQ(info.mode & S_IFMT, static_cast<std::uint32_t>(S_IFREG));
    EXPECT_EQ(info.uid, sys.effective_uid());
    EXPECT_NE(info.inode, 0U);

    wsrv::FileInfo through_link;
    wsrv::FileInfo link_itself;
    ASSERT_EQ(sys.stat_path(link.c_str(), true, through_link).err, 0);
    ASSERT_EQ(sys.stat_path(link.c_str(), false, link_itself).err, 0);
    EXPECT_TRUE(through_link.same_file_as(info));
    EXPECT_EQ(link_itself.mode & S_IFMT, static_cast<std::uint32_t>(S_IFLNK));
    EXPECT_FALSE(link_itself.same_file_as(info));

    EXPECT_EQ(sys.stat_path((pattern + "/missing").c_str(), false, info).err, ENOENT);

    ASSERT_EQ(sys.chmod_path(file.c_str(), 0640).err, 0);
    ASSERT_EQ(sys.stat_path(file.c_str(), false, info).err, 0);
    EXPECT_EQ(info.mode & 0777U, 0640U);
    EXPECT_TRUE(info.same_file_as(through_link)) << "chmod keeps the identity";

    EXPECT_EQ(sys.unlink_path(file.c_str()).err, 0);
    EXPECT_EQ(sys.unlink_path(file.c_str()).err, ENOENT);
    std::error_code ignored;
    fs::remove_all(pattern, ignored);
}

TEST(LinuxSyscalls, ConnectReportsRefusalForASocketFileNobodyListensOn) {
    namespace fs = std::filesystem;
    wsrv::LinuxSyscalls sys;
    std::string pattern = (fs::temp_directory_path() / "wsrv-sys-XXXXXX").string();
    ASSERT_NE(::mkdtemp(pattern.data()), nullptr);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const std::string path = pattern + "/s";
    std::copy(path.begin(), path.end(), static_cast<char*>(address.sun_path));
    const int server = ::socket(AF_UNIX, SOCK_STREAM, 0);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    const auto* generic = reinterpret_cast<const sockaddr*>(&address);
    ASSERT_EQ(::bind(server, generic, sizeof(address)), 0);
    ASSERT_EQ(::listen(server, 1), 0);
    ::close(server);  // the file stays, nobody answers

    const int client = ::socket(AF_UNIX, SOCK_STREAM, 0);
    EXPECT_EQ(sys.connect(client, generic, sizeof(address)).err, ECONNREFUSED);
    ::close(client);
    std::error_code ignored;
    fs::remove_all(pattern, ignored);
}

TEST(LinuxSyscalls, PeerUidIsTheUidOfTheOtherEndOfAUnixSocket) {
    wsrv::LinuxSyscalls sys;
    std::array<int, 2> pair{};
    ASSERT_EQ(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair.data()), 0);
    std::uint32_t uid = 99999;
    EXPECT_EQ(sys.peer_uid(pair[0], uid).err, 0);
    EXPECT_EQ(uid, sys.effective_uid());  // both ends are this process
    ::close(pair[0]);
    ::close(pair[1]);
    EXPECT_EQ(sys.peer_uid(pair[0], uid).err, EBADF);
}

TEST(FakeSyscalls, ScriptsThePeerUid) {
    FakeSyscalls sys;
    sys.succeed(Call::PeerUid, 4242);
    std::uint32_t uid = 0;
    EXPECT_EQ(sys.peer_uid(-1, uid).err, 0);
    EXPECT_EQ(uid, 4242U);
}

TEST(FakeSyscalls, ScriptsTheFilesystemCallsAndTheEffectiveUid) {
    FakeSyscalls sys;
    sys.succeed(Call::EffectiveUid, 4242);
    EXPECT_EQ(sys.effective_uid(), 4242U);
    EXPECT_NE(sys.effective_uid(), 4242U) << "a scripted value is used once";
    sys.fail(Call::UnlinkPath, EPERM);
    EXPECT_EQ(sys.unlink_path("/nonexistent/wsrv").err, EPERM);
    EXPECT_EQ(sys.unlink_path("/nonexistent/wsrv").err, ENOENT);
    EXPECT_EQ(sys.calls(Call::UnlinkPath), 2U);
}

}  // namespace
