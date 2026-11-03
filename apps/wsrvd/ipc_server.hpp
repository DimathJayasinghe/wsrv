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

#include "wsrvd/handshake.hpp"

namespace wsrv::wsrvd {

struct IpcServerConfig {
    std::string socket_path;
    std::string token;            ///< empty = no token required (the uid check still applies)
    std::uint32_t max_frame = 0;  ///< largest frame after HELLO; also the outbound buffer's unit
    std::chrono::milliseconds hello_timeout{5000};
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
///  4. After HELLO_OK the decoder is widened to max_frame. For now only PING (answered with PONG)
///     is understood; any other frame closes the connection. Commands and events are slice 3.
/// A malformed or oversized frame closes the connection with no reply: a length-prefixed stream
/// cannot be resynchronised.
///
/// Thread-affinity: loop-thread-only; create() must run on the loop's thread.
class IpcServer final : public IEventHandler {
public:
    /// @return Error from UnixListener::open (unsafe path, socket in use...) or from the loop.
    [[nodiscard]] static Result<std::unique_ptr<IpcServer>>
    create(EventLoop& loop, ISyscalls& sys, IpcServerConfig config);

    class Key {
        friend class IpcServer;
        Key() = default;
    };
    IpcServer(Key /*unused*/,
              EventLoop& loop,
              ISyscalls& sys,
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

    void accept_burst() noexcept;
    void admit(Fd client) noexcept;
    void reject_busy(Fd client) noexcept;
    void schedule_retry() noexcept;
    void release(const Peer* peer) noexcept;

    EventLoop& loop_;
    ISyscalls& sys_;
    IpcServerConfig config_;
    Handshake handshake_;
    UnixListener listener_;
    std::optional<WatchHandle> watch_;
    std::optional<TimerHandle> retry_;
    std::unique_ptr<Peer> peer_;
    /// Lifetime token for tasks handed to the loop: they hold a weak_ptr, so a task that outlives
    /// the server does nothing.
    std::shared_ptr<char> epoch_ = std::make_shared<char>();
};

}  // namespace wsrv::wsrvd
