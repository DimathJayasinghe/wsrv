// SPDX-License-Identifier: Apache-2.0
#include "net/syscalls.hpp"

#include <cerrno>

#include <fcntl.h>
#include <sys/eventfd.h>
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

SysResult LinuxSyscalls::send(int fd, const void* buf, std::size_t count, int flags) {
    return captured(::send(fd, buf, count, flags));
}

SysResult LinuxSyscalls::shutdown(int fd, int how) {
    return captured(::shutdown(fd, how));
}

SysResult LinuxSyscalls::setsockopt(int fd, int level, int name, const void* value, socklen_t len) {
    return captured(::setsockopt(fd, level, name, value, len));
}

SysResult LinuxSyscalls::getsockname(int fd, sockaddr* addr, socklen_t* len) {
    return captured(::getsockname(fd, addr, len));
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

void LinuxSyscalls::close(int fd) noexcept {
    ::close(fd);  // see ISyscalls::close: never retried
}

}  // namespace wsrv
