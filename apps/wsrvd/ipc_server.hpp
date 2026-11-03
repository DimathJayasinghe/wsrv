// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/event_loop.hpp"
#include "net/syscalls.hpp"
#include "net/unix_listener.hpp"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "wsrvd/backend.hpp"
#include "wsrvd/handshake.hpp"
#include "wsrvd/relay.hpp"

namespace wsrv::wsrvd {

struct IpcServerConfig {
    std::string socket_path;
    std::string token;            ///< empty = no token required (the uid check still applies)
    std::uint32_t max_frame = 0;  ///< largest frame after HELLO; also the outbound buffer's unit
    std::chrono::milliseconds hello_timeout{5000};
    /// How long the application may leave our output unread before the link is closed.
    std::chrono::milliseconds stall_timeout{10000};
    /// After an application disconnects, how long its connections may wait for a replacement
    /// before they are closed with 1012. (main passes the server's shutdown_grace.)
    std::chrono::milliseconds restart_grace{10000};
};

/// The IPC side of the daemon (ADR-0017): a Unix socket that ONE application connects to.
///
/// What happens to a connection, in order:
///  1. accept. The kernel's record of who connected (SO_PEERCRED) is read BEFORE any byte: a peer
///     whose uid is not the daemon's is dropped without a reply.
///  2. If an application is already connected, the newcomer gets STATUS(request 0,
///     WSRV_ERR_BACKPRESSURE) and is closed: the single consumer slot is full (ADR-0017 4a).
///  3. HELLO must arrive within hello_timeout, in a frame no larger than the biggest legal HELLO.
///     Wrong version, wrong token or any other first frame: one STATUS(request 0), then close.
///  4. After HELLO_OK the decoder is widened to max_frame. Events flow out (the pump), commands flow
///     in and each is answered with a STATUS (Relay). PING is answered with PONG. Any frame type
///     only the daemon may send, and a second HELLO, close the connection.
///  5. While no application is connected the daemon takes nothing from the backend: events wait in
///     its queue, whose limits pause the WebSocket clients (they are held, not refused). When an
///     application disconnects, the connections it knew are closed with 1012 after restart_grace
///     (their application state is gone, so they cannot be served by the next one). Connections it
///     never saw wait for the next application. An application cut off for stalling has its
///     connections closed with 1001 at once.
/// A malformed or oversized frame closes the connection with no reply: a length-prefixed stream
/// cannot be resynchronised.
///
/// Thread-affinity: loop-thread-only; create() must run on the loop's thread.
class IpcServer final : public IEventHandler {
public:
    /// @return Error from UnixListener::open (unsafe path, socket in use...) or from the loop.
    [[nodiscard]] static Result<std::unique_ptr<IpcServer>>
    create(EventLoop& loop, ISyscalls& sys, IBackend& backend, IpcServerConfig config);

    class Key {
        friend class IpcServer;
        Key() = default;
    };
    IpcServer(Key /*unused*/,
              EventLoop& loop,
              ISyscalls& sys,
              IBackend& backend,
              IpcServerConfig config,
              UnixListener listener);
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) = delete;
    IpcServer& operator=(IpcServer&&) = delete;
    ~IpcServer() override;

    /// Listener readiness: accept new connections.
    void on_event(PollEvents events) noexcept override;

    /// True while an application is connected (accepted or not yet through HELLO).
    [[nodiscard]] bool has_consumer() const noexcept;

private:
    class Peer;

    /// The backend's event_fd became readable: events are waiting.
    class EventSource final : public IEventHandler {
    public:
        explicit EventSource(IpcServer& server) noexcept : server_{server} {}
        void on_event(PollEvents /*events*/) noexcept override {
            server_.on_backend_events();
        }

    private:
        IpcServer& server_;
    };

    void on_backend_events() noexcept;
    /// The application that had completed HELLO is gone (`stalled`: we cut it off because it
    /// stopped reading). Decide what happens to the connections it knew.
    void application_lost(bool stalled) noexcept;
    /// Ask the loop to report the backend's event_fd again (it is edge-triggered): used when the
    /// pump stops with events still queued but wants to come back on a later loop iteration.
    void rearm_event_source() noexcept;

    void accept_burst() noexcept;
    void admit(Fd client) noexcept;
    void reject_busy(Fd client) noexcept;
    void schedule_retry() noexcept;
    void release(const Peer* peer) noexcept;

    EventLoop& loop_;
    ISyscalls& sys_;
    IpcServerConfig config_;
    Handshake handshake_;
    Relay relay_;
    UnixListener listener_;
    EventSource event_source_{*this};
    std::optional<WatchHandle> watch_;
    std::optional<WatchHandle> events_watch_;
    std::optional<TimerHandle> retry_;
    std::optional<TimerHandle> grace_timer_;  ///< armed while stale connections await 1012
    std::unique_ptr<Peer> peer_;
    /// Lifetime token for tasks handed to the loop: they hold a weak_ptr, so a task that outlives
    /// the server does nothing.
    std::shared_ptr<char> epoch_ = std::make_shared<char>();
};

}  // namespace wsrv::wsrvd
