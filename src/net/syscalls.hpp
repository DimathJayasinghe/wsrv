// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/io_error.hpp"

#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include <poll.h>
#include <sys/epoll.h>
#include <sys/socket.h>

namespace wsrv {

/// Outcome of one kernel call: `err == 0` -> `value` is the call's (non-negative) result;
/// otherwise `value == -1` and `err` is the `errno` captured immediately after the call
/// (CODING_STANDARDS: never read errno later).
struct SysResult {
    long value = 0;
    int err = 0;
};

/// Socket option identifier: the (level, name) pair of setsockopt(2).
struct SockOpt {
    int level;
    int name;
};

/// What the server needs to know about a file, in plain integers so no caller touches `struct stat`
/// (whose fields differ per OS) and a test can describe a file without creating one.
struct FileInfo {
    std::uint32_t mode = 0;  ///< st_mode: file type and permission bits
    std::uint32_t uid = 0;   ///< owner
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    /// device + inode name one file for as long as it exists; a replaced file gets a new inode.
    /// (Not operator==: the mode legitimately changes, for instance after chmod.)
    [[nodiscard]] bool same_file_as(const FileInfo& other) const noexcept {
        return device == other.device && inode == other.inode;
    }
};

/// RLIMIT_NOFILE in plain integers: how many descriptors this process may hold. "Unlimited"
/// (RLIM_INFINITY) is the largest uint64, so a comparison with a need is always right.
struct FdLimits {
    std::uint64_t soft = 0;  ///< the limit in force: open()/accept() fail with EMFILE beyond it
    std::uint64_t hard = 0;  ///< the ceiling an unprivileged process may raise `soft` to
};

/// Every kernel call the server makes, behind one interface (docs/TESTING_STRATEGY.md section 2,
/// "Syscall layer"). Production code uses LinuxSyscalls; tests use a fake that injects failures
/// (EINTR, EAGAIN, EMFILE, EPIPE...) so every error path runs without a misbehaving kernel.
/// The interface is deliberately thin: one method per libc call, no policy. EINTR retries and
/// errno mapping live in `retrying()` / `map_errno()`, not here.
///
/// Thread-affinity: implementations are any-thread (they hold no state of their own).
class ISyscalls {
public:
    ISyscalls() = default;
    ISyscalls(const ISyscalls&) = delete;
    ISyscalls& operator=(const ISyscalls&) = delete;
    ISyscalls(ISyscalls&&) = delete;
    ISyscalls& operator=(ISyscalls&&) = delete;
    virtual ~ISyscalls() = default;

    virtual SysResult socket(int domain, int type, int protocol) = 0;
    virtual SysResult bind(int fd, const sockaddr* addr, socklen_t len) = 0;
    virtual SysResult listen(int fd, int backlog) = 0;
    virtual SysResult accept4(int fd, sockaddr* peer, socklen_t* peer_len, int flags) = 0;
    virtual SysResult read(int fd, void* buf, std::size_t count) = 0;
    virtual SysResult write(int fd, const void* buf, std::size_t count) = 0;
    /// poll(2). The logger asks with a zero timeout whether a descriptor can take a line
    /// (ADR-0037).
    virtual SysResult poll(pollfd* fds, nfds_t count, int timeout_ms) = 0;
    virtual SysResult send(int fd, const void* buf, std::size_t count, int flags) = 0;
    virtual SysResult shutdown(int fd, int how) = 0;
    virtual SysResult setsockopt(int fd, SockOpt option, const void* value, socklen_t len) = 0;
    virtual SysResult getsockname(int fd, sockaddr* addr, socklen_t* len) = 0;
    virtual SysResult connect(int fd, const sockaddr* addr, socklen_t len) = 0;
    /// stat(2) when `follow_links`, lstat(2) otherwise. `path` is NUL-terminated. On success `info`
    /// is filled in.
    virtual SysResult stat_path(const char* path, bool follow_links, FileInfo& info) = 0;
    virtual SysResult chmod_path(const char* path, std::uint32_t mode) = 0;
    virtual SysResult unlink_path(const char* path) = 0;
    /// open(2) with the given flags (O_CLOEXEC is always added) and, when it creates the file, the
    /// given mode (before the umask, which can only remove bits). Used to create a secret file with
    /// O_EXCL, so that an existing file or a symlink is never overwritten or followed.
    virtual SysResult open_file(const char* path, int flags, std::uint32_t mode) = 0;
    /// The uid of the process on the other end of a connected Unix-domain socket, as the kernel
    /// recorded it when the connection was made (SO_PEERCRED on Linux; macOS would use
    /// LOCAL_PEERCRED). It cannot be forged by the peer. On success `uid` is filled in.
    virtual SysResult peer_uid(int fd, std::uint32_t& uid) = 0;
    /// geteuid(2): cannot fail.
    virtual std::uint32_t effective_uid() noexcept = 0;
    /// A copy of `fd` with close-on-exec, numbered `lowest` or above (fcntl F_DUPFD_CLOEXEC). The
    /// logger keeps its own copy of stderr this way (ADR-0037).
    virtual SysResult dup_cloexec(int fd, int lowest) = 0;
    virtual SysResult epoll_create1(int flags) = 0;
    virtual SysResult epoll_ctl(int epoll_fd, int op, int fd, epoll_event* event) = 0;
    virtual SysResult
    epoll_wait(int epoll_fd, epoll_event* events, int max_events, int timeout_ms) = 0;
    virtual SysResult eventfd(unsigned initial, int flags) = 0;
    /// Open /dev/null read-only with O_CLOEXEC: the spare descriptor the acceptor keeps for the
    /// EMFILE trick (EC-N05).
    virtual SysResult open_null() = 0;
    /// pthread_sigmask(2) for the calling thread. Note the libc call reports its error as a return
    /// value, not errno; the wrapper folds it into `err` like every other call.
    virtual SysResult thread_sigmask(int how, const sigset_t* set, sigset_t* old_set) = 0;
    /// signalfd(2): `fd == -1` creates a new descriptor.
    virtual SysResult signalfd(int fd, const sigset_t* mask, int flags) = 0;
    /// getrlimit(RLIMIT_NOFILE) (ADR-0022).
    virtual SysResult get_fd_limits(FdLimits& limits) = 0;
    /// setrlimit(RLIMIT_NOFILE) changing only the soft limit; the hard limit is kept as it is.
    virtual SysResult set_fd_soft_limit(std::uint64_t soft) = 0;
    /// Close without retrying on EINTR (on Linux the descriptor is gone either way).
    virtual void close(int fd) noexcept = 0;
};

/// The real kernel.
class LinuxSyscalls final : public ISyscalls {
public:
    SysResult socket(int domain, int type, int protocol) override;
    SysResult bind(int fd, const sockaddr* addr, socklen_t len) override;
    SysResult listen(int fd, int backlog) override;
    SysResult accept4(int fd, sockaddr* peer, socklen_t* peer_len, int flags) override;
    SysResult read(int fd, void* buf, std::size_t count) override;
    SysResult write(int fd, const void* buf, std::size_t count) override;
    SysResult poll(pollfd* fds, nfds_t count, int timeout_ms) override;
    SysResult send(int fd, const void* buf, std::size_t count, int flags) override;
    SysResult shutdown(int fd, int how) override;
    SysResult setsockopt(int fd, SockOpt option, const void* value, socklen_t len) override;
    SysResult getsockname(int fd, sockaddr* addr, socklen_t* len) override;
    SysResult connect(int fd, const sockaddr* addr, socklen_t len) override;
    SysResult stat_path(const char* path, bool follow_links, FileInfo& info) override;
    SysResult chmod_path(const char* path, std::uint32_t mode) override;
    SysResult unlink_path(const char* path) override;
    SysResult open_file(const char* path, int flags, std::uint32_t mode) override;
    SysResult peer_uid(int fd, std::uint32_t& uid) override;
    std::uint32_t effective_uid() noexcept override;
    SysResult dup_cloexec(int fd, int lowest) override;
    SysResult epoll_create1(int flags) override;
    SysResult epoll_ctl(int epoll_fd, int op, int fd, epoll_event* event) override;
    SysResult
    epoll_wait(int epoll_fd, epoll_event* events, int max_events, int timeout_ms) override;
    SysResult eventfd(unsigned initial, int flags) override;
    SysResult open_null() override;
    SysResult thread_sigmask(int how, const sigset_t* set, sigset_t* old_set) override;
    SysResult signalfd(int fd, const sigset_t* mask, int flags) override;
    SysResult get_fd_limits(FdLimits& limits) override;
    SysResult set_fd_soft_limit(std::uint64_t soft) override;
    void close(int fd) noexcept override;
};

/// A signal can interrupt a slow call before it did anything (EINTR); the right response is to
/// call again. Bounded, so a pathological signal storm cannot spin the loop forever.
inline constexpr std::size_t kMaxEintrRetries = 64;

/// Run `call` (returning SysResult) until it stops reporting EINTR, then convert: success ->
/// the value; any other errno -> `map_errno(err, context)`. After kMaxEintrRetries interruptions
/// the result is Error{Io, context}. `context` must be a string literal.
template <class Call>
[[nodiscard]] Result<long> retrying(Call&& call, std::string_view context) {
    for (std::size_t attempt = 0; attempt < kMaxEintrRetries; ++attempt) {
        const SysResult result = call();
        if (result.err == EINTR) {
            continue;
        }
        if (result.err != 0) {
            return std::unexpected{map_errno(result.err, context)};
        }
        return result.value;
    }
    return fail(ErrorCode::Io, context);
}

}  // namespace wsrv
