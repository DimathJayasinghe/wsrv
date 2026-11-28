// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "base/logger.hpp"
#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/syscalls.hpp"
#include "net/tcp_listener.hpp"
#include "net/unix_listener.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <string>
#include <variant>

#include "wsrvd/backend.hpp"
#include "wsrvd/handshake.hpp"
#include "wsrvd/relay.hpp"

namespace wsrv::wsrvd {

/// The listening socket of either transport (ADR-0038).
using IpcListener = std::variant<UnixListener, TcpListener>;

struct IpcServerConfig {
    /// EXACTLY ONE of these is set. A Unix socket path (the peer's uid is checked)...
    std::string socket_path;
    /// ...or a TCP address to bind (port 0 = the kernel chooses; see IpcServer::tcp_endpoint()).
    /// Over TCP there is no peer uid, so the token is mandatory and a connection that has not said
    /// HELLO does not hold the single application slot (ADR-0038 section 5).
    std::optional<Endpoint> tcp;
    std::string token;            ///< empty = no token required on a Unix socket (never over TCP)
    std::uint32_t max_frame = 0;  ///< largest frame after HELLO; also the outbound buffer's unit
    std::chrono::milliseconds hello_timeout{5000};
    /// Over TCP, after the daemon's last word to a peer it turned away (a busy refusal, a rejected
    /// HELLO): how long it keeps the half-closed socket, discarding input, so that a reset cannot
    /// wipe the status the peer has not read yet (Windows drops received data on RST). It ends
    /// sooner when the peer closes. ADR-0038 section 5.
    std::chrono::milliseconds linger_timeout{2000};
    /// How long the application may leave our output unread before the link is closed.
    std::chrono::milliseconds stall_timeout{10000};
    /// After an application disconnects, how long its connections may wait for a replacement
    /// before they are closed with 1012. (main passes the server's shutdown_grace.)
    std::chrono::milliseconds restart_grace{10000};
    /// The WebSocket server asks the application about every upgrade (ADR-0018). The daemon then
    /// requires protocol minor 2 in HELLO and keeps the backend's intake open only while an
    /// application is connected, so a browser never waits for an answer nobody can give.
    bool handshake_hook = false;
    /// Where the daemon says who connected, who was refused and who left (ADR-0037). Null = silent.
    /// Never given a token or any peer-supplied text.
    ILogger* logger = nullptr;
};

/// The IPC side of the daemon (ADR-0017): a Unix socket that ONE application connects to.
///
/// What happens to a connection, in order:
///  1. accept. On a Unix socket the kernel's record of who connected (SO_PEERCRED) is read BEFORE any
///     byte: a peer whose uid is not the daemon's is dropped without a reply. Over TCP there is no
///     such record; the token (mandatory) is the authentication. A connection that has not said
///     HELLO yet waits in a room of kMaxPendingHello places, not in the application's slot; the
///     first to send an accepted HELLO becomes the application and the others are refused; when the
///     room is full the oldest is evicted (ADR-0038 section 5).
///  2. If an application is already connected, the newcomer gets STATUS(request 0,
///     WSRV_ERR_BACKPRESSURE) and is closed: the single consumer slot is full (ADR-0017 4a).
///  3. HELLO must arrive within hello_timeout, in a frame no larger than the biggest legal HELLO.
///     Wrong version, wrong token or any other first frame: one STATUS(request 0), then close.
///  4. After HELLO_OK the decoder is widened to max_frame. Events flow out (the pump), commands
///  flow
///     in and each is answered with a STATUS (Relay). PING is answered with PONG. Any frame type
///     only the daemon may send, and a second HELLO, close the connection.
///     With handshake_hook, a Handshake event (kind 6) asks the application about an upgrade and
///     HANDSHAKE_ACCEPT / HANDSHAKE_REJECT answer it. An event too big for a frame is refused with
///     503 instead of closing the link: its size is the client's doing, not the application's.
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
    /// @return Error{InvalidConfig} unless exactly one transport is configured, or for a TCP
    /// transport without a token; the listener's error (unsafe path, address in use...) or the
    /// loop's otherwise.
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
              IpcListener listener);
    IpcServer(const IpcServer&) = delete;
    IpcServer& operator=(const IpcServer&) = delete;
    IpcServer(IpcServer&&) = delete;
    IpcServer& operator=(IpcServer&&) = delete;
    ~IpcServer() override;

    /// Listener readiness: accept new connections.
    void on_event(PollEvents events) noexcept override;

    /// True while an application is connected (on a Unix socket also while it has not yet said
    /// HELLO; over TCP a connection that has not is waiting, not a consumer).
    [[nodiscard]] bool has_consumer() const noexcept;

    /// How many TCP connections may wait for their HELLO at once (ADR-0038 section 5). To keep the
    /// real application out an attacker must make more than this many connections inside the window
    /// between its connect and its HELLO being read. Public so that the tests can say "a full room".
    static constexpr std::size_t kMaxPendingHello = 8;

    /// The address the TCP transport is bound to (the real port when 0 was asked); nothing for a
    /// Unix socket.
    [[nodiscard]] std::optional<Endpoint> tcp_endpoint() const noexcept;

private:
    class Peer;
    class Lingering;
    /// Sockets kept open after our last word (linger()). Each is a descriptor and a small object
    /// for at most `linger_timeout`; a flood of refused connections can fill this, and the rest are
    /// then closed at once (the status was sent; only a Windows peer may lose it to a reset).
    static constexpr std::size_t kMaxLingering = 32;

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
    /// Open or close the backend's door for handshake questions (a no-op without the hook).
    void open_intake(bool open) noexcept;
    /// A question that cannot be put to the application: answer the browser 503.
    void refuse_oversized(const HostEvent& question) noexcept;
    /// The application that had completed HELLO is gone (`stalled`: we cut it off because it
    /// stopped reading). Decide what happens to the connections it knew.
    void application_lost(bool stalled) noexcept;
    /// Ask the loop to report the backend's event_fd again (it is edge-triggered): used when the
    /// pump stops with events still queued but wants to come back on a later loop iteration (when
    /// the queue's wake-up is still owed, the loop that owes it supplies the edge; ADR-0035).
    void rearm_event_source() noexcept;

    void accept_burst() noexcept;
    void admit(Fd client) noexcept;
    /// TCP: put a connection that has not said HELLO into the waiting room, evicting the oldest one
    /// when it is full.
    void admit_waiting(Fd client) noexcept;
    /// A waiting connection's HELLO was accepted: make it THE application and refuse every other
    /// waiting connection. False if it cannot (another one was accepted first).
    [[nodiscard]] bool claim(Peer& peer) noexcept;
    /// Tell every waiting connection BACKPRESSURE and let it go (an application was accepted).
    void refuse_waiting() noexcept;
    /// Take the waiting connection at `index` out of the room (the ones after it move up).
    [[nodiscard]] std::unique_ptr<Peer> take_waiting(std::size_t index) noexcept;
    /// Drop waiting connections that have closed themselves (timeout, EOF) and wait to be released.
    void forget_closed_waiting() noexcept;
    [[nodiscard]] int listener_fd() const noexcept;
    [[nodiscard]] bool is_tcp() const noexcept;
    /// TCP_NODELAY and keepalive probes on an accepted TCP socket (ADR-0038 section 5).
    void tune_tcp(const Fd& client) noexcept;
    /// Answer BACKPRESSURE to a connection we cannot take, then linger() it.
    void reject_busy(Fd client) noexcept;
    /// Over TCP, keep a socket we have finished talking to (FIN already sent) until its peer closes
    /// or `linger_timeout` passes, discarding what arrives, so that closing it cannot make the
    /// operating system send RST and wipe the last status from a peer that has not read it yet
    /// (Windows does). At most kMaxLingering at a time; beyond that the socket is closed at once.
    /// A Unix socket has no such problem: this does nothing for one (the Fd closes).
    void linger(Fd fd) noexcept;
    void release_lingering(const Lingering* entry) noexcept;
    void schedule_retry() noexcept;
    /// Close every stale connection; those a loop refuses are tried again shortly (ADR-0034).
    void close_stale_connections(std::uint16_t code, std::string_view reason);
    void release(const Peer* peer) noexcept;
    /// One log line; a logger that throws never fails the daemon.
    void note(LogLevel level, std::string_view event, std::string_view message) const noexcept;

    EventLoop& loop_;
    ISyscalls& sys_;
    IBackend& backend_;
    IpcServerConfig config_;
    Handshake handshake_;
    Relay relay_;
    IpcListener listener_;
    EventSource event_source_{*this};
    std::optional<WatchHandle> watch_;
    std::optional<WatchHandle> events_watch_;
    std::optional<TimerHandle> retry_;
    std::optional<TimerHandle> grace_timer_;  ///< armed while stale connections await 1012
    std::optional<TimerHandle> close_retry_;  ///< armed while a loop has refused a stale close
    std::unique_ptr<Peer> peer_;  ///< the application (on a Unix socket, also before its HELLO)
    /// TCP only: connections that have not said HELLO, oldest first, `waiting_count_` of them.
    /// Fixed slots: no allocation, and a flood cannot grow it.
    std::array<std::unique_ptr<Peer>, kMaxPendingHello> waiting_;
    std::size_t waiting_count_ = 0;
    std::array<std::unique_ptr<Lingering>, kMaxLingering> lingering_;  ///< fixed slots: no allocation
    /// Lifetime token for tasks handed to the loop: they hold a weak_ptr, so a task that outlives
    /// the server does nothing.
    std::shared_ptr<char> epoch_ = std::make_shared<char>();
};

}  // namespace wsrv::wsrvd
