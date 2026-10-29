// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/fd.hpp"
#include "net/poller.hpp"
#include "net/syscalls.hpp"

#include <memory>

namespace wsrv {

/// IPoller on Linux epoll, edge-triggered (ADR-0004). All kernel calls go through ISyscalls.
/// Thread-affinity: loop-thread-only.
class EpollPoller final : public IPoller {
public:
    /// @return Error{FdLimit} if no descriptor is available, Error{Io} otherwise.
    [[nodiscard]] static Result<std::unique_ptr<EpollPoller>> create(ISyscalls& sys);

    /// Construction goes through create(); the passkey keeps this constructor unusable elsewhere
    /// while still letting std::make_unique call it (no raw `new`).
    class Key {
        friend class EpollPoller;
        Key() = default;
    };
    EpollPoller(Key /*unused*/, ISyscalls& sys, Fd epoll_fd)
        : sys_{sys}, epoll_fd_{std::move(epoll_fd)} {}

    [[nodiscard]] Result<void> add(int fd, PollEvents interest, std::uint64_t token) override;
    [[nodiscard]] Result<void> modify(int fd, PollEvents interest, std::uint64_t token) override;
    [[nodiscard]] Result<void> remove(int fd) override;
    [[nodiscard]] Result<std::size_t> wait(int timeout_ms, std::span<PollEvent> out) override;

private:
    [[nodiscard]] Result<void> control(int op, int fd, PollEvents interest, std::uint64_t token);

    ISyscalls& sys_;
    Fd epoll_fd_;
};

}  // namespace wsrv
