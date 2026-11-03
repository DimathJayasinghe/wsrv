// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/syscalls.hpp"

#include <array>
#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace wsrv::test {

/// Which kernel call a script entry or a counter belongs to.
enum class Call : std::size_t {
    Socket,
    Bind,
    Listen,
    Accept4,
    Read,
    Write,
    Send,
    Shutdown,
    SetSockOpt,
    GetSockName,
    Connect,
    StatPath,
    ChmodPath,
    UnlinkPath,
    EffectiveUid,
    EpollCreate1,
    EpollCtl,
    EpollWait,
    EventFd,
    OpenNull,
    SigMask,
    SignalFd,
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
    [[nodiscard]] std::size_t calls(Call call) const {
        const std::lock_guard<std::mutex> lock{mutex_};
        return count_[index(call)];
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
        return real_.write(fd, buf, n);
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
    SysResult unlink_path(const char* path) override {
        if (const auto injected = scripted(Call::UnlinkPath)) {
            return *injected;
        }
        return real_.unlink_path(path);
    }
    /// A scripted `succeed(Call::EffectiveUid, uid)` makes the next call report that uid.
    std::uint32_t effective_uid() noexcept override {
        if (const auto injected = scripted(Call::EffectiveUid)) {
            return static_cast<std::uint32_t>(injected->value);
        }
        return real_.effective_uid();
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
};

}  // namespace wsrv::test
