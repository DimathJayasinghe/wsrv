// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/fd.hpp"
#include "net/syscalls.hpp"
#include "net/tcp_listener.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>

namespace wsrv {

struct AcceptorConfig {
    /// Most connections accepted per loop iteration; the rest wait for the next iteration so other
    /// connections are served in between (EC-N08).
    std::size_t max_accepts_per_event = 64;
    /// How long accepting pauses after running out of descriptors (or after an unexpected accept
    /// error), so the loop does not spin on a condition that cannot clear instantly (EC-N05).
    std::chrono::milliseconds emfile_backoff{100};
};

/// Accepts connections from a listener inside the event loop and hands each one to a callback.
///
///  - accept4() makes every accepted socket non-blocking and close-on-exec; TCP_NODELAY is set.
///  - EINTR is retried; EAGAIN ends the burst quietly; ECONNABORTED / EPROTO (the client gave up
///    before we accepted) are skipped without ending the burst (EC-N06).
///  - Out of descriptors (EMFILE / ENFILE): the *reserve descriptor* trick. The acceptor keeps one
///    spare descriptor (/dev/null). On exhaustion it closes the spare, accepts the waiting
///    connection(s) into the freed slot and closes them straight away ("shed": the client sees a
///    close instead of hanging in the kernel queue), reopens the spare, then stops watching the
///    listener for `emfile_backoff` (EC-N05, SECURITY_DESIGN T3).
///  - Edge-triggered listener: when the per-iteration budget is used up, the acceptor re-arms its
///    registration (rewatch) so the kernel reports the still-queued connections in the next
///    iteration, after every other ready descriptor has had its turn.
///
/// The Acceptor must be destroyed (or stopped) before the EventLoop and the TcpListener it uses.
/// The callback runs on the loop thread, must not throw and must take ownership of the Fd.
/// Thread-affinity: loop-thread-only.
class Acceptor final : public IEventHandler {
public:
    using AcceptCallback = std::function<void(Fd client, Endpoint peer)>;

    Acceptor(EventLoop& loop,
             TcpListener& listener,
             ISyscalls& sys,
             AcceptCallback on_accept,
             AcceptorConfig config);
    ~Acceptor() override;

    /// Open the reserve descriptor and start watching the listener.
    [[nodiscard]] Result<void> start();
    /// Stop watching (queued clients stay in the kernel queue) and cancel a pending back-off.
    void stop();

    /// Connections accepted only to be closed because the process was out of descriptors.
    [[nodiscard]] std::uint64_t shed_count() const noexcept {
        return shed_count_;
    }
    /// Unexpected accept errors (each triggers a back-off).
    [[nodiscard]] std::uint64_t error_count() const noexcept {
        return error_count_;
    }

    void on_event(PollEvents events) noexcept override;

private:
    [[nodiscard]] SysResult accept_one(sockaddr_storage& peer);
    void accept_burst() noexcept;
    void deliver(Fd client, const sockaddr_storage& peer) noexcept;
    void shed_pending() noexcept;
    void pause() noexcept;
    void schedule_backoff() noexcept;
    void resume() noexcept;

    EventLoop& loop_;
    TcpListener& listener_;
    ISyscalls& sys_;
    AcceptCallback on_accept_;
    AcceptorConfig config_;
    Fd reserve_;
    std::optional<WatchHandle> watch_;
    std::optional<TimerHandle> backoff_;
    /// Lifetime token for tasks posted to the loop: they hold a weak_ptr, and stop() / the
    /// destructor drop this one, so a task that outlives the acceptor does nothing.
    std::shared_ptr<char> epoch_ = std::make_shared<char>();
    std::uint64_t shed_count_ = 0;
    std::uint64_t error_count_ = 0;
};

}  // namespace wsrv
