// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/syscalls.hpp"

#include <array>
#include <cstddef>
#include <deque>
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
    EpollCreate1,
    EpollCtl,
    EpollWait,
    EventFd,
    OpenNull,
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
        for (std::size_t i = 0; i < times; ++i) {
            script_[index(call)].push_back(SysResult{-1, err});
        }
    }
    /// The next call of `call` returns `value` without reaching the kernel.
    void succeed(Call call, long value) {
        script_[index(call)].push_back(SysResult{value, 0});
    }
    [[nodiscard]] std::size_t calls(Call call) const {
        return count_[index(call)];
    }
    [[nodiscard]] std::size_t pending_script(Call call) const {
        return script_[index(call)].size();
    }

    SysResult socket(int domain, int type, int protocol) override {
        return scripted(Call::Socket) ? *taken_ : real_.socket(domain, type, protocol);
    }
    SysResult bind(int fd, const sockaddr* addr, socklen_t len) override {
        return scripted(Call::Bind) ? *taken_ : real_.bind(fd, addr, len);
    }
    SysResult listen(int fd, int backlog) override {
        return scripted(Call::Listen) ? *taken_ : real_.listen(fd, backlog);
    }
    SysResult accept4(int fd, sockaddr* peer, socklen_t* peer_len, int flags) override {
        return scripted(Call::Accept4) ? *taken_ : real_.accept4(fd, peer, peer_len, flags);
    }
    SysResult read(int fd, void* buf, std::size_t n) override {
        return scripted(Call::Read) ? *taken_ : real_.read(fd, buf, n);
    }
    SysResult write(int fd, const void* buf, std::size_t n) override {
        return scripted(Call::Write) ? *taken_ : real_.write(fd, buf, n);
    }
    SysResult send(int fd, const void* buf, std::size_t n, int flags) override {
        return scripted(Call::Send) ? *taken_ : real_.send(fd, buf, n, flags);
    }
    SysResult shutdown(int fd, int how) override {
        return scripted(Call::Shutdown) ? *taken_ : real_.shutdown(fd, how);
    }
    SysResult setsockopt(int fd, int level, int name, const void* v, socklen_t len) override {
        return scripted(Call::SetSockOpt) ? *taken_ : real_.setsockopt(fd, level, name, v, len);
    }
    SysResult getsockname(int fd, sockaddr* addr, socklen_t* len) override {
        return scripted(Call::GetSockName) ? *taken_ : real_.getsockname(fd, addr, len);
    }
    SysResult epoll_create1(int flags) override {
        return scripted(Call::EpollCreate1) ? *taken_ : real_.epoll_create1(flags);
    }
    SysResult epoll_ctl(int ep, int op, int fd, epoll_event* event) override {
        return scripted(Call::EpollCtl) ? *taken_ : real_.epoll_ctl(ep, op, fd, event);
    }
    SysResult epoll_wait(int ep, epoll_event* events, int max, int timeout_ms) override {
        return scripted(Call::EpollWait) ? *taken_ : real_.epoll_wait(ep, events, max, timeout_ms);
    }
    SysResult eventfd(unsigned initial, int flags) override {
        return scripted(Call::EventFd) ? *taken_ : real_.eventfd(initial, flags);
    }
    SysResult open_null() override {
        return scripted(Call::OpenNull) ? *taken_ : real_.open_null();
    }
    void close(int fd) noexcept override {
        ++count_[index(Call::Close)];
        real_.close(fd);
    }

private:
    static constexpr std::size_t index(Call call) {
        return static_cast<std::size_t>(call);
    }
    /// Count the call; if a result is scripted, take it into `taken_` and return true.
    bool scripted(Call call) {
        ++count_[index(call)];
        std::deque<SysResult>& queue = script_[index(call)];
        if (queue.empty()) {
            taken_.reset();
            return false;
        }
        taken_ = queue.front();
        queue.pop_front();
        return true;
    }

    LinuxSyscalls real_;
    std::array<std::deque<SysResult>, static_cast<std::size_t>(Call::Count)> script_;
    std::array<std::size_t, static_cast<std::size_t>(Call::Count)> count_{};
    std::optional<SysResult> taken_;
};

}  // namespace wsrv::test
