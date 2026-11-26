// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/syscalls.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace wsrv::test {

/// Which kernel call a script entry or a counter belongs to.
enum class Call : std::size_t {
    Socket,
    Bind,
    Listen,
    Accept4,
    Read,
    Write,
    Poll,
    Send,
    Shutdown,
    SetSockOpt,
    GetSockName,
    Connect,
    StatPath,
    ChmodPath,
    UnlinkPath,
    PeerUid,
    EffectiveUid,
    DupCloexec,
    EpollCreate1,
    EpollCtl,
    EpollWait,
    EventFd,
    OpenNull,
    SigMask,
    SignalFd,
    GetFdLimits,
    SetFdSoftLimit,
    OpenFile,
    Close,
    Count
};

/// The real kernel with failure injection: a test queues outcomes for a call ("accept4 fails
/// with EMFILE once"); calls with an empty queue go to the real syscall. Every call is counted.
/// This is the "Syscall layer with fault injection" of docs/TESTING_STRATEGY.md section 2.
class FakeSyscalls final : public ISyscalls {
public:
    /// The next `times` calls of `call` fail with `err` (value -1) without reaching the kernel.
    void fail(Call call, int err, std::size_t times = 1) {
        const std::lock_guard<std::mutex> lock{mutex_};
        for (std::size_t i = 0; i < times; ++i) {
            script_[index(call)].push_back(SysResult{-1, err});
        }
    }
    /// The next call of `call` returns `value` without reaching the kernel.
    void succeed(Call call, long value) {
        const std::lock_guard<std::mutex> lock{mutex_};
        script_[index(call)].push_back(SysResult{value, 0});
    }
    /// The next write(2) really writes at most `bytes` bytes of its buffer (a short write): the
    /// caller must go on with the rest. Queue several to shorten several writes in a row.
    void short_write(std::size_t bytes) {
        const std::lock_guard<std::mutex> lock{mutex_};
        short_writes_.push_back(bytes);
    }
    /// From now on getrlimit/setrlimit(RLIMIT_NOFILE) act on this in-memory pair, never on the
    /// real process: a test can describe any host limit, and raising the soft limit above `hard`
    /// is refused with EINVAL like the kernel does (soft > hard). Without this call the real limit
    /// is used.
    void pretend_fd_limits(std::uint64_t soft, std::uint64_t hard) {
        const std::lock_guard<std::mutex> lock{mutex_};
        pretended_ = FdLimits{soft, hard};
    }
    [[nodiscard]] FdLimits pretended_fd_limits() const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return pretended_.value_or(FdLimits{});
    }
    [[nodiscard]] std::size_t calls(Call call) const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return count_[index(call)];
    }
    /// One setsockopt(2) call whose value was an int (all the options the server sets are).
    struct SockOptCall {
        int level = 0;
        int name = 0;
        int value = 0;
        friend bool operator==(const SockOptCall&, const SockOptCall&) = default;
    };
    /// Every int-valued setsockopt call so far, in order (a test checks what the server asked for;
    /// the kernel still gets the call, so the option really is set).
    [[nodiscard]] std::vector<SockOptCall> sockopt_calls() const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return sockopts_;
    }
    [[nodiscard]] std::size_t pending_script(Call call) const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return script_[index(call)].size();
    }

    SysResult socket(int domain, int type, int protocol) override {
        if (const auto injected = scripted(Call::Socket)) {
            return *injected;
        }
        return real_.socket(domain, type, protocol);
    }
    SysResult bind(int fd, const sockaddr* addr, socklen_t len) override {
        if (const auto injected = scripted(Call::Bind)) {
            return *injected;
        }
        return real_.bind(fd, addr, len);
    }
    SysResult listen(int fd, int backlog) override {
        if (const auto injected = scripted(Call::Listen)) {
            return *injected;
        }
        return real_.listen(fd, backlog);
    }
    SysResult accept4(int fd, sockaddr* peer, socklen_t* peer_len, int flags) override {
        if (const auto injected = scripted(Call::Accept4)) {
            return *injected;
        }
        return real_.accept4(fd, peer, peer_len, flags);
    }
    SysResult read(int fd, void* buf, std::size_t n) override {
        if (const auto injected = scripted(Call::Read)) {
            return *injected;
        }
        return real_.read(fd, buf, n);
    }
    SysResult write(int fd, const void* buf, std::size_t n) override {
        if (const auto injected = scripted(Call::Write)) {
            return *injected;
        }
        const std::lock_guard<std::mutex> lock{mutex_};
        if (!short_writes_.empty()) {
            // A real write of at most the queued size: the kernel taking only part of the buffer.
            const std::size_t limit = short_writes_.front();
            short_writes_.pop_front();
            return real_.write(fd, buf, n < limit ? n : limit);
        }
        return real_.write(fd, buf, n);
    }
    SysResult poll(pollfd* fds, nfds_t count, int timeout_ms) override {
        if (const auto injected = scripted(Call::Poll)) {
            return *injected;
        }
        return real_.poll(fds, count, timeout_ms);
    }
    SysResult send(int fd, const void* buf, std::size_t n, int flags) override {
        if (const auto injected = scripted(Call::Send)) {
            return *injected;
        }
        return real_.send(fd, buf, n, flags);
    }
    SysResult shutdown(int fd, int how) override {
        if (const auto injected = scripted(Call::Shutdown)) {
            return *injected;
        }
        return real_.shutdown(fd, how);
    }
    SysResult setsockopt(int fd, SockOpt option, const void* v, socklen_t len) override {
        if (len == sizeof(int)) {
            int value = 0;
            std::memcpy(&value, v, sizeof(int));
            const std::lock_guard<std::mutex> lock{mutex_};
            sockopts_.push_back(SockOptCall{option.level, option.name, value});
        }
        if (const auto injected = scripted(Call::SetSockOpt)) {
            return *injected;
        }
        return real_.setsockopt(fd, option, v, len);
    }
    SysResult getsockname(int fd, sockaddr* addr, socklen_t* len) override {
        if (const auto injected = scripted(Call::GetSockName)) {
            return *injected;
        }
        return real_.getsockname(fd, addr, len);
    }
    SysResult connect(int fd, const sockaddr* addr, socklen_t len) override {
        if (const auto injected = scripted(Call::Connect)) {
            return *injected;
        }
        return real_.connect(fd, addr, len);
    }
    SysResult stat_path(const char* path, bool follow_links, FileInfo& info) override {
        if (const auto injected = scripted(Call::StatPath)) {
            return *injected;
        }
        return real_.stat_path(path, follow_links, info);
    }
    SysResult chmod_path(const char* path, std::uint32_t mode) override {
        if (const auto injected = scripted(Call::ChmodPath)) {
            return *injected;
        }
        return real_.chmod_path(path, mode);
    }
    SysResult open_file(const char* path, int flags, std::uint32_t mode) override {
        if (const auto injected = scripted(Call::OpenFile)) {
            return *injected;
        }
        return real_.open_file(path, flags, mode);
    }
    SysResult unlink_path(const char* path) override {
        if (const auto injected = scripted(Call::UnlinkPath)) {
            return *injected;
        }
        return real_.unlink_path(path);
    }
    /// A scripted `succeed(Call::PeerUid, uid)` reports that uid as the peer's.
    SysResult peer_uid(int fd, std::uint32_t& uid) override {
        if (const auto injected = scripted(Call::PeerUid)) {
            if (injected->err == 0) {
                uid = static_cast<std::uint32_t>(injected->value);
            }
            return *injected;
        }
        return real_.peer_uid(fd, uid);
    }
    /// A scripted `succeed(Call::EffectiveUid, uid)` makes the next call report that uid.
    std::uint32_t effective_uid() noexcept override {
        if (const auto injected = scripted(Call::EffectiveUid)) {
            return static_cast<std::uint32_t>(injected->value);
        }
        return real_.effective_uid();
    }
    SysResult dup_cloexec(int fd, int lowest) override {
        if (const auto injected = scripted(Call::DupCloexec)) {
            return *injected;
        }
        return real_.dup_cloexec(fd, lowest);
    }
    SysResult epoll_create1(int flags) override {
        if (const auto injected = scripted(Call::EpollCreate1)) {
            return *injected;
        }
        return real_.epoll_create1(flags);
    }
    SysResult epoll_ctl(int ep, int op, int fd, epoll_event* event) override {
        if (const auto injected = scripted(Call::EpollCtl)) {
            return *injected;
        }
        return real_.epoll_ctl(ep, op, fd, event);
    }
    SysResult epoll_wait(int ep, epoll_event* events, int max, int timeout_ms) override {
        if (const auto injected = scripted(Call::EpollWait)) {
            return *injected;
        }
        return real_.epoll_wait(ep, events, max, timeout_ms);
    }
    SysResult eventfd(unsigned initial, int flags) override {
        if (const auto injected = scripted(Call::EventFd)) {
            return *injected;
        }
        return real_.eventfd(initial, flags);
    }
    SysResult open_null() override {
        if (const auto injected = scripted(Call::OpenNull)) {
            return *injected;
        }
        return real_.open_null();
    }
    SysResult thread_sigmask(int how, const sigset_t* set, sigset_t* old_set) override {
        if (const auto injected = scripted(Call::SigMask)) {
            return *injected;
        }
        return real_.thread_sigmask(how, set, old_set);
    }
    SysResult signalfd(int fd, const sigset_t* mask, int flags) override {
        if (const auto injected = scripted(Call::SignalFd)) {
            return *injected;
        }
        return real_.signalfd(fd, mask, flags);
    }
    SysResult get_fd_limits(FdLimits& limits) override {
        if (const auto injected = scripted(Call::GetFdLimits)) {
            return *injected;
        }
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (pretended_) {
                limits = *pretended_;
                return SysResult{0, 0};
            }
        }
        return real_.get_fd_limits(limits);
    }
    SysResult set_fd_soft_limit(std::uint64_t soft) override {
        if (const auto injected = scripted(Call::SetFdSoftLimit)) {
            return *injected;
        }
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            if (pretended_) {
                if (soft > pretended_->hard) {
                    return SysResult{-1, EINVAL};
                }
                pretended_->soft = soft;
                return SysResult{0, 0};
            }
        }
        return real_.set_fd_soft_limit(soft);
    }
    void close(int fd) noexcept override {
        {
            const std::lock_guard<std::mutex> lock{mutex_};
            ++count_[index(Call::Close)];
        }
        real_.close(fd);
    }

private:
    static constexpr std::size_t index(Call call) {
        return static_cast<std::size_t>(call);
    }
    /// Count the call and, if a result is scripted for it, take that result. Thread-safe: the
    /// event loop calls write() (its wake-up) from any thread.
    std::optional<SysResult> scripted(Call call) {
        const std::lock_guard<std::mutex> lock{mutex_};
        ++count_[index(call)];
        std::deque<SysResult>& queue = script_[index(call)];
        if (queue.empty()) {
            return std::nullopt;
        }
        const SysResult result = queue.front();
        queue.pop_front();
        return result;
    }

    LinuxSyscalls real_;
    mutable std::mutex mutex_;
    std::array<std::deque<SysResult>, static_cast<std::size_t>(Call::Count)> script_;
    std::array<std::size_t, static_cast<std::size_t>(Call::Count)> count_{};
    std::vector<SockOptCall> sockopts_;
    std::optional<FdLimits> pretended_;
    std::deque<std::size_t> short_writes_;
};

}  // namespace wsrv::test
