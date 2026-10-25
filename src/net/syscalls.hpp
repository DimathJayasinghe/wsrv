// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/io_error.hpp"

#include <cerrno>
#include <cstddef>
#include <string_view>

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
    virtual SysResult send(int fd, const void* buf, std::size_t count, int flags) = 0;
    virtual SysResult shutdown(int fd, int how) = 0;
    virtual SysResult setsockopt(int fd, SockOpt option, const void* value, socklen_t len) = 0;
    virtual SysResult getsockname(int fd, sockaddr* addr, socklen_t* len) = 0;
    virtual SysResult epoll_create1(int flags) = 0;
    virtual SysResult epoll_ctl(int epoll_fd, int op, int fd, epoll_event* event) = 0;
    virtual SysResult
    epoll_wait(int epoll_fd, epoll_event* events, int max_events, int timeout_ms) = 0;
    virtual SysResult eventfd(unsigned initial, int flags) = 0;
    /// Open /dev/null read-only with O_CLOEXEC: the spare descriptor the acceptor keeps for the
    /// EMFILE trick (EC-N05).
    virtual SysResult open_null() = 0;
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
    SysResult send(int fd, const void* buf, std::size_t count, int flags) override;
    SysResult shutdown(int fd, int how) override;
    SysResult setsockopt(int fd, SockOpt option, const void* value, socklen_t len) override;
    SysResult getsockname(int fd, sockaddr* addr, socklen_t* len) override;
    SysResult epoll_create1(int flags) override;
    SysResult epoll_ctl(int epoll_fd, int op, int fd, epoll_event* event) override;
    SysResult
    epoll_wait(int epoll_fd, epoll_event* events, int max_events, int timeout_ms) override;
    SysResult eventfd(unsigned initial, int flags) override;
    SysResult open_null() override;
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
