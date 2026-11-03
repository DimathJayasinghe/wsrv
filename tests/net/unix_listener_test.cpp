// SPDX-License-Identifier: Apache-2.0
#include "net/unix_listener.hpp"

#include "support/fake_syscalls.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace {

namespace fs = std::filesystem;
using wsrv::ErrorCode;
using wsrv::Fd;
using wsrv::UnixListener;
using wsrv::UnixListenerConfig;
using wsrv::test::Call;
using wsrv::test::FakeSyscalls;

/// A private (mode 0700) directory that is removed with everything in it.
class TempDir {
public:
    TempDir() {
        std::string pattern = (fs::temp_directory_path() / "wsrv-unix-XXXXXX").string();
        const char* made = ::mkdtemp(pattern.data());
        EXPECT_NE(made, nullptr);
        path_ = pattern;
        ::chmod(path_.c_str(), 0700);
    }
    TempDir(const TempDir&) = delete;
    TempDir& operator=(const TempDir&) = delete;
    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    [[nodiscard]] std::string file(const std::string& name) const {
        return path_ + "/" + name;
    }
    [[nodiscard]] const std::string& path() const {
        return path_;
    }

private:
    std::string path_;
};

const sockaddr* as_generic(const sockaddr_un& address) {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<const sockaddr*>(&address);
}

/// connect() to a socket path; the descriptor is valid on success.
Fd connect_to(const std::string& path, int* error = nullptr) {
    Fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    const int rc = ::connect(fd.get(), as_generic(address), sizeof(address));
    if (error != nullptr) {
        *error = rc == 0 ? 0 : errno;
    }
    if (rc != 0) {
        return Fd{};
    }
    return fd;
}

/// A socket file with nobody behind it: what a crashed daemon leaves.
void make_stale_socket(const std::string& path) {
    const Fd fd{::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0)};
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
    ASSERT_EQ(::bind(fd.get(), as_generic(address), sizeof(address)), 0);
    ASSERT_EQ(::listen(fd.get(), 1), 0);
    // closing the descriptor leaves the file but nothing accepts: connect() gets ECONNREFUSED
}

struct UnixListenerTest : public ::testing::Test {
    FakeSyscalls sys;
    TempDir dir;
    [[nodiscard]] UnixListenerConfig config(const std::string& name = "ipc.sock") const {
        return UnixListenerConfig{.path = dir.file(name)};
    }
};

mode_t mode_of(const std::string& path) {
    struct stat info {};
    EXPECT_EQ(::lstat(path.c_str(), &info), 0);
    return info.st_mode;
}

TEST_F(UnixListenerTest, OpensASocketOnlyTheOwnerCanReach) {  // ADR-0017 section 4
    auto listener = UnixListener::open(sys, config());
    ASSERT_TRUE(listener.has_value());
    const mode_t mode = mode_of(listener->path());
    EXPECT_TRUE(S_ISSOCK(mode));
    EXPECT_EQ(mode & 0777U, 0600U);
}

TEST_F(UnixListenerTest, TheSocketIsNonBlockingAndCloseOnExec) {
    auto listener = UnixListener::open(sys, config());
    ASSERT_TRUE(listener.has_value());
    EXPECT_NE(::fcntl(listener->fd(), F_GETFL) & O_NONBLOCK, 0);
    EXPECT_NE(::fcntl(listener->fd(), F_GETFD) & FD_CLOEXEC, 0);
}

TEST_F(UnixListenerTest, AClientCanConnectAndBeAccepted) {
    auto listener = UnixListener::open(sys, config());
    ASSERT_TRUE(listener.has_value());
    const Fd client = connect_to(listener->path());
    ASSERT_TRUE(client.valid());
    const int accepted = ::accept4(listener->fd(), nullptr, nullptr, SOCK_CLOEXEC);
    EXPECT_GE(accepted, 0);
    ::close(accepted);
}

TEST_F(UnixListenerTest, TheFileIsRemovedWhenTheListenerIsDestroyed) {
    std::string path;
    {
        auto listener = UnixListener::open(sys, config());
        ASSERT_TRUE(listener.has_value());
        path = listener->path();
        ASSERT_TRUE(fs::exists(path));
    }
    EXPECT_FALSE(fs::exists(path));
}

TEST_F(UnixListenerTest, AMovedListenerRemovesTheFileExactlyOnce) {
    auto first = UnixListener::open(sys, config());
    ASSERT_TRUE(first.has_value());
    const std::string path = first->path();
    {
        const UnixListener second = std::move(*first);
        EXPECT_TRUE(fs::exists(path));
    }
    EXPECT_FALSE(fs::exists(path));
    EXPECT_EQ(sys.calls(Call::UnlinkPath), 1U);
}

TEST_F(UnixListenerTest, AFileSomeoneElseCreatedAtThePathIsNotRemoved) {
    const std::string path = config().path;
    {
        auto listener = UnixListener::open(sys, config());
        ASSERT_TRUE(listener.has_value());
        ASSERT_EQ(::unlink(path.c_str()), 0);
        std::ofstream{path} << "not ours";
    }
    EXPECT_TRUE(fs::exists(path)) << "the destructor must not delete a replaced file";
}

// ---- the path itself --------------------------------------------------------------------------

TEST_F(UnixListenerTest, RejectsAnEmptyPath) {
    const auto listener = UnixListener::open(sys, UnixListenerConfig{.path = ""});
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
}

TEST_F(UnixListenerTest, RejectsAnAbstractNamespaceName) {  // leading NUL: no file permissions
    const auto listener =
        UnixListener::open(sys, UnixListenerConfig{.path = std::string{"\0wsrv", 5}});
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
}

TEST_F(UnixListenerTest, RejectsAnEmbeddedNul) {  // would silently become a shorter path
    const auto listener =
        UnixListener::open(sys, UnixListenerConfig{.path = dir.file("a") + std::string{"\0b", 2}});
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
}

TEST_F(UnixListenerTest, RejectsAPathThatDoesNotFitAndNeverTruncatesIt) {
    const sockaddr_un probe{};
    const std::string too_long = dir.file(std::string(sizeof(probe.sun_path), 'x'));
    const auto listener = UnixListener::open(sys, UnixListenerConfig{.path = too_long});
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
    EXPECT_EQ(sys.calls(Call::Bind), 0U) << "nothing may be bound with a truncated name";
}

// ---- the directory ----------------------------------------------------------------------------

TEST_F(UnixListenerTest, RefusesAGroupWritableDirectory) {
    ASSERT_EQ(::chmod(dir.path().c_str(), 0770), 0);
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
    EXPECT_EQ(sys.calls(Call::Bind), 0U);
}

TEST_F(UnixListenerTest, RefusesAWorldWritableDirectoryEvenWithTheStickyBit) {
    ASSERT_EQ(::chmod(dir.path().c_str(), 01777), 0);  // like /tmp
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
}

TEST_F(UnixListenerTest, AGroupReadableButNotWritableDirectoryIsFine) {
    ASSERT_EQ(::chmod(dir.path().c_str(), 0755), 0);
    EXPECT_TRUE(UnixListener::open(sys, config()).has_value());
}

TEST_F(UnixListenerTest, RefusesADirectoryOwnedBySomeoneElse) {
    sys.succeed(Call::EffectiveUid,
                4242);  // we pretend to be another user: the directory is not ours
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
    EXPECT_EQ(sys.calls(Call::Bind), 0U);
}

TEST_F(UnixListenerTest, ADirectoryThatDoesNotExistIsAConfigurationError) {
    const auto listener =
        UnixListener::open(sys, UnixListenerConfig{.path = dir.file("no/such/ipc.sock")});
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
}

TEST_F(UnixListenerTest, TheDirectoryIsCheckedBeforeAnythingIsRemoved) {
    const std::string path = config().path;
    make_stale_socket(path);
    ASSERT_EQ(::chmod(dir.path().c_str(), 0777), 0);
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_TRUE(fs::exists(path)) << "an unsafe directory must stop us before we unlink";
    EXPECT_EQ(sys.calls(Call::UnlinkPath), 0U);
}

// ---- what is already at the path --------------------------------------------------------------

TEST_F(UnixListenerTest, ReplacesAStaleSocketLeftByACrash) {
    make_stale_socket(config().path);
    auto listener = UnixListener::open(sys, config());
    ASSERT_TRUE(listener.has_value()) << listener.error().context;
    EXPECT_TRUE(connect_to(listener->path()).valid());
}

TEST_F(UnixListenerTest, RefusesToReplaceASocketThatAnswers) {
    auto first = UnixListener::open(sys, config());
    ASSERT_TRUE(first.has_value());
    const auto second = UnixListener::open(sys, config());
    ASSERT_FALSE(second.has_value());
    EXPECT_EQ(second.error().code, ErrorCode::AddressInUse);
    EXPECT_TRUE(connect_to(first->path()).valid()) << "the running daemon must be undisturbed";
}

TEST_F(UnixListenerTest, RefusesWhenTheProbeIsInconclusive) {  // EAGAIN: backlog full = alive
    make_stale_socket(config().path);
    sys.fail(Call::Connect, EAGAIN);
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_TRUE(fs::exists(config().path));
    EXPECT_EQ(sys.calls(Call::UnlinkPath), 0U);
}

TEST_F(UnixListenerTest, RefusesAnythingElseTheProbeReports) {
    make_stale_socket(config().path);
    sys.fail(Call::Connect, EACCES);
    EXPECT_FALSE(UnixListener::open(sys, config()).has_value());
    EXPECT_TRUE(fs::exists(config().path));
}

TEST_F(UnixListenerTest, NeverRemovesARegularFile) {
    std::ofstream{config().path} << "precious";
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
    EXPECT_TRUE(fs::is_regular_file(config().path));
}

TEST_F(UnixListenerTest, NeverFollowsASymlinkAtThePath) {
    make_stale_socket(dir.file("real.sock"));
    ASSERT_EQ(::symlink(dir.file("real.sock").c_str(), config().path.c_str()), 0);
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
    EXPECT_TRUE(fs::is_symlink(config().path));
    EXPECT_TRUE(fs::exists(dir.file("real.sock"))) << "the link's target must be untouched";
}

TEST_F(UnixListenerTest, NeverRemovesASocketOwnedBySomeoneElse) {
    make_stale_socket(config().path);
    sys.succeed(Call::EffectiveUid, 4242);  // we pretend to be another user: the file is not ours
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::InvalidConfig);
    EXPECT_TRUE(fs::exists(config().path));
}

// ---- failures while creating ------------------------------------------------------------------

TEST_F(UnixListenerTest, AChmodFailureLeavesNoSocketFileBehind) {
    sys.fail(Call::ChmodPath, EPERM);
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_FALSE(fs::exists(config().path));
}

TEST_F(UnixListenerTest, AListenFailureLeavesNoSocketFileBehind) {
    sys.fail(Call::Listen, EADDRINUSE);
    ASSERT_FALSE(UnixListener::open(sys, config()).has_value());
    EXPECT_FALSE(fs::exists(config().path));
}

TEST_F(UnixListenerTest, NobodyCanConnectBeforeThePermissionsAreSet) {
    // The order is bind, chmod, listen. If chmod fails, listen was never reached, so the file that
    // briefly existed with default permissions could never accept a connection.
    sys.fail(Call::ChmodPath, EPERM);
    ASSERT_FALSE(UnixListener::open(sys, config()).has_value());
    EXPECT_EQ(sys.calls(Call::Listen), 0U);
}

TEST_F(UnixListenerTest, OutOfDescriptorsIsReportedAsFdLimit) {
    sys.fail(Call::Socket, EMFILE);
    const auto listener = UnixListener::open(sys, config());
    ASSERT_FALSE(listener.has_value());
    EXPECT_EQ(listener.error().code, ErrorCode::FdLimit);
}

std::size_t open_descriptors() {
    std::size_t count = 0;
    for (const auto& entry : fs::directory_iterator{"/proc/self/fd"}) {
        (void)entry;
        ++count;
    }
    return count;
}

TEST_F(UnixListenerTest, NoDescriptorLeaksOnAnyFailurePath) {
    const std::size_t before = open_descriptors();
    sys.fail(Call::ChmodPath, EPERM);
    EXPECT_FALSE(UnixListener::open(sys, config()).has_value());
    sys.fail(Call::Listen, EADDRINUSE);
    EXPECT_FALSE(UnixListener::open(sys, config()).has_value());
    sys.fail(Call::Bind, EACCES);
    EXPECT_FALSE(UnixListener::open(sys, config()).has_value());
    EXPECT_EQ(open_descriptors(), before);
}

}  // namespace
