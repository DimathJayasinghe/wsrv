// SPDX-License-Identifier: Apache-2.0
#include "net/syscalls.hpp"

#include <cerrno>
#include <limits>

#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <unistd.h>

namespace wsrv {

namespace {

/// Turn a libc return value into a SysResult. Called with the libc call as its argument, so
/// errno is read before anything else can touch it.
[[nodiscard]] SysResult captured(long result) noexcept {
    if (result < 0) {
        return SysResult{-1, errno};
    }
    return SysResult{result, 0};
}

}  // namespace

SysResult LinuxSyscalls::socket(int domain, int type, int protocol) {
    return captured(::socket(domain, type, protocol));
}

SysResult LinuxSyscalls::bind(int fd, const sockaddr* addr, socklen_t len) {
    return captured(::bind(fd, addr, len));
}

SysResult LinuxSyscalls::listen(int fd, int backlog) {
    return captured(::listen(fd, backlog));
}

SysResult LinuxSyscalls::accept4(int fd, sockaddr* peer, socklen_t* peer_len, int flags) {
    return captured(::accept4(fd, peer, peer_len, flags));
}

SysResult LinuxSyscalls::read(int fd, void* buf, std::size_t count) {
    return captured(::read(fd, buf, count));
}

SysResult LinuxSyscalls::write(int fd, const void* buf, std::size_t count) {
    return captured(::write(fd, buf, count));
}

SysResult LinuxSyscalls::poll(pollfd* fds, nfds_t count, int timeout_ms) {
    return captured(::poll(fds, count, timeout_ms));
}

SysResult LinuxSyscalls::send(int fd, const void* buf, std::size_t count, int flags) {
    return captured(::send(fd, buf, count, flags));
}

SysResult LinuxSyscalls::shutdown(int fd, int how) {
    return captured(::shutdown(fd, how));
}

SysResult LinuxSyscalls::setsockopt(int fd, SockOpt option, const void* value, socklen_t len) {
    return captured(::setsockopt(fd, option.level, option.name, value, len));
}

SysResult LinuxSyscalls::getsockname(int fd, sockaddr* addr, socklen_t* len) {
    return captured(::getsockname(fd, addr, len));
}

SysResult LinuxSyscalls::connect(int fd, const sockaddr* addr, socklen_t len) {
    return captured(::connect(fd, addr, len));
}

SysResult LinuxSyscalls::stat_path(const char* path, bool follow_links, FileInfo& info) {
    struct stat raw {};
    const int rc = follow_links ? ::stat(path, &raw) : ::lstat(path, &raw);
    const SysResult result = captured(rc);
    if (result.err == 0) {
        info.mode = static_cast<std::uint32_t>(raw.st_mode);
        info.uid = static_cast<std::uint32_t>(raw.st_uid);
        info.device = static_cast<std::uint64_t>(raw.st_dev);
        info.inode = static_cast<std::uint64_t>(raw.st_ino);
    }
    return result;
}

SysResult LinuxSyscalls::chmod_path(const char* path, std::uint32_t mode) {
    return captured(::chmod(path, static_cast<mode_t>(mode)));
}

SysResult LinuxSyscalls::unlink_path(const char* path) {
    return captured(::unlink(path));
}

SysResult LinuxSyscalls::open_file(const char* path, int flags, std::uint32_t mode) {
    return captured(::open(path, flags | O_CLOEXEC, static_cast<mode_t>(mode)));
}

SysResult LinuxSyscalls::peer_uid(int fd, std::uint32_t& uid) {
    ucred credentials{};
    socklen_t length = sizeof(credentials);
    const SysResult result =
        captured(::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length));
    if (result.err == 0) {
        uid = static_cast<std::uint32_t>(credentials.uid);
    }
    return result;
}

std::uint32_t LinuxSyscalls::effective_uid() noexcept {
    return static_cast<std::uint32_t>(::geteuid());
}

SysResult LinuxSyscalls::dup_cloexec(int fd, int lowest) {
    return captured(::fcntl(fd, F_DUPFD_CLOEXEC, lowest));
}

SysResult LinuxSyscalls::epoll_create1(int flags) {
    return captured(::epoll_create1(flags));
}

SysResult LinuxSyscalls::epoll_ctl(int epoll_fd, int op, int fd, epoll_event* event) {
    return captured(::epoll_ctl(epoll_fd, op, fd, event));
}

SysResult
LinuxSyscalls::epoll_wait(int epoll_fd, epoll_event* events, int max_events, int timeout_ms) {
    return captured(::epoll_wait(epoll_fd, events, max_events, timeout_ms));
}

SysResult LinuxSyscalls::eventfd(unsigned initial, int flags) {
    return captured(::eventfd(initial, flags));
}

SysResult LinuxSyscalls::open_null() {
    return captured(::open("/dev/null", O_RDONLY | O_CLOEXEC));
}

SysResult LinuxSyscalls::thread_sigmask(int how, const sigset_t* set, sigset_t* old_set) {
    const int err =
        ::pthread_sigmask(how, set, old_set);  // returns the error number, sets no errno
    return err == 0 ? SysResult{0, 0} : SysResult{-1, err};
}

namespace {

/// RLIM_INFINITY is "no limit": the largest value, whatever width the platform gives rlim_t.
[[nodiscard]] std::uint64_t from_rlim(rlim_t value) noexcept {
    return value == RLIM_INFINITY ? std::numeric_limits<std::uint64_t>::max()
                                  : static_cast<std::uint64_t>(value);
}

[[nodiscard]] rlim_t to_rlim(std::uint64_t value) noexcept {
    return value >= static_cast<std::uint64_t>(RLIM_INFINITY) ? RLIM_INFINITY
                                                              : static_cast<rlim_t>(value);
}

}  // namespace

SysResult LinuxSyscalls::get_fd_limits(FdLimits& limits) {
    rlimit current{};
    const SysResult result = captured(::getrlimit(RLIMIT_NOFILE, &current));
    if (result.err == 0) {
        limits = FdLimits{from_rlim(current.rlim_cur), from_rlim(current.rlim_max)};
    }
    return result;
}

SysResult LinuxSyscalls::set_fd_soft_limit(std::uint64_t soft) {
    rlimit current{};
    if (const SysResult read = captured(::getrlimit(RLIMIT_NOFILE, &current)); read.err != 0) {
        return read;
    }
    current.rlim_cur = to_rlim(soft);  // rlim_max stays: only the soft limit is ours to change
    return captured(::setrlimit(RLIMIT_NOFILE, &current));
}

SysResult LinuxSyscalls::signalfd(int fd, const sigset_t* mask, int flags) {
    return captured(::signalfd(fd, mask, flags));
}

void LinuxSyscalls::close(int fd) noexcept {
    ::close(fd);  // see ISyscalls::close: never retried
}

}  // namespace wsrv
