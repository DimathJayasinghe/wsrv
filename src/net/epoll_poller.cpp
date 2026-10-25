// SPDX-License-Identifier: Apache-2.0
#include "net/epoll_poller.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <new>

#include <sys/epoll.h>

namespace wsrv {

namespace {

/// Largest batch asked of the kernel in one call; a bigger `out` just takes several calls.
constexpr std::size_t kMaxBatch = 256;

[[nodiscard]] std::uint32_t to_epoll(PollEvents interest) noexcept {
    std::uint32_t bits = EPOLLET;  // edge-triggered, always
    if (has(interest, PollEvents::Read)) {
        bits |= EPOLLIN | EPOLLRDHUP;
    }
    if (has(interest, PollEvents::Write)) {
        bits |= EPOLLOUT;
    }
    return bits;
}

[[nodiscard]] PollEvents from_epoll(std::uint32_t bits) noexcept {
    PollEvents events = PollEvents::None;
    if ((bits & EPOLLIN) != 0) {
        events = events | PollEvents::Read;
    }
    if ((bits & EPOLLOUT) != 0) {
        events = events | PollEvents::Write;
    }
    if ((bits & EPOLLERR) != 0) {
        events = events | PollEvents::Error;
    }
    if ((bits & (EPOLLHUP | EPOLLRDHUP)) != 0) {
        events = events | PollEvents::Hangup;
    }
    return events;
}

}  // namespace

Result<std::unique_ptr<EpollPoller>> EpollPoller::create(ISyscalls& sys) {
    const Result<long> epoll_fd = retrying([&] { return sys.epoll_create1(EPOLL_CLOEXEC); }, "epoll_create1");
    if (!epoll_fd) {
        return std::unexpected{epoll_fd.error()};
    }
    Fd owned{static_cast<int>(*epoll_fd)};
    try {
        return std::make_unique<EpollPoller>(Key{}, sys, std::move(owned));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "epoll poller: out of memory");
    }
}

Result<void> EpollPoller::control(int op, int fd, PollEvents interest, std::uint64_t token) {
    epoll_event event{};
    event.events = to_epoll(interest);
    event.data.u64 = token;
    const Result<long> done =
        retrying([&] { return sys_.epoll_ctl(epoll_fd_.get(), op, fd, &event); }, "epoll_ctl");
    if (!done) {
        return std::unexpected{done.error()};
    }
    return {};
}

Result<void> EpollPoller::add(int fd, PollEvents interest, std::uint64_t token) {
    return control(EPOLL_CTL_ADD, fd, interest, token);
}

Result<void> EpollPoller::modify(int fd, PollEvents interest, std::uint64_t token) {
    return control(EPOLL_CTL_MOD, fd, interest, token);
}

Result<void> EpollPoller::remove(int fd) {
    return control(EPOLL_CTL_DEL, fd, PollEvents::None, 0);
}

Result<std::size_t> EpollPoller::wait(int timeout_ms, std::span<PollEvent> out) {
    if (out.empty()) {
        return std::size_t{0};
    }
    std::array<epoll_event, kMaxBatch> raw{};
    const auto room = static_cast<int>(std::min(out.size(), raw.size()));
    const SysResult result = sys_.epoll_wait(epoll_fd_.get(), raw.data(), room, timeout_ms);
    if (result.err == EINTR) {
        return std::size_t{0};  // a signal woke us: the caller loops anyway (EC-N01)
    }
    if (result.err != 0) {
        return std::unexpected{map_errno(result.err, "epoll_wait")};
    }
    const auto count = static_cast<std::size_t>(result.value);
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = PollEvent{raw[i].data.u64, from_epoll(raw[i].events)};
    }
    return count;
}

}  // namespace wsrv
