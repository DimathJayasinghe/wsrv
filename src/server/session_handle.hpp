// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/event_loop.hpp"
#include "server/handler.hpp"
#include "server/handshake_verdict.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <string_view>

namespace wsrv {

/// Something to do with a session, run on the loop thread.
using SessionCommand = std::function<void(Session&)>;

/// Runs commands on live connections by id. Implemented by the ConnectionTable.
/// Thread-affinity: loop-thread-only (it is only ever called from a posted task).
class ISessionDirectory {
public:
    ISessionDirectory() = default;
    ISessionDirectory(const ISessionDirectory&) = delete;
    ISessionDirectory& operator=(const ISessionDirectory&) = delete;
    ISessionDirectory(ISessionDirectory&&) = delete;
    ISessionDirectory& operator=(ISessionDirectory&&) = delete;
    virtual ~ISessionDirectory() = default;

    /// Run `command` on connection `id` if it is alive, then let the connection write what the
    /// command queued (a posted command is not inside a handler callback, so nothing else would
    /// flush it). A missing connection is not an error: the command is dropped.
    virtual void run(ConnectionId id, const SessionCommand& command) = 0;

    /// Deliver the application's answer to the upgrade of connection `id`. It acts only if that
    /// connection exists and is still waiting for a decision; in every other case (gone, already
    /// decided, never asked) it is dropped without effect (ADR-0018 section 3).
    virtual void decide(ConnectionId id, const HandshakeVerdict& verdict) = 0;
};

/// The one object shared between a loop and every thread that holds a SessionHandle (ADR-0014
/// section 4). It lets a foreign thread post a command to the loop and lets the loop's owner
/// cut the connection off *before* the loop and the directory are destroyed.
///
/// The mutex guards only the pointers and is held for exactly one post() (a bounded queue push):
/// there is no lock in Connection or ProtocolSession. A posted command runs only while the loop
/// runs, which is while the directory is alive: close() is called before either is destroyed, and
/// tasks still queued at that point are discarded with the loop, never executed.
/// Thread-affinity: any thread.
class LoopLink {
public:
    LoopLink(EventLoop& loop, ISessionDirectory& directory) noexcept
        : loop_{&loop}, directory_{&directory} {}
    LoopLink(const LoopLink&) = delete;
    LoopLink& operator=(const LoopLink&) = delete;
    LoopLink(LoopLink&&) = delete;
    LoopLink& operator=(LoopLink&&) = delete;
    ~LoopLink() = default;

    /// Run `command` on the loop thread with the session `id`, if it still exists then.
    /// @return Error{Closing} after close(), Error{BufferFull} when the loop's task queue is full
    /// (EC-N14), Error{ResourceExhausted} on allocation failure. Ok means "accepted for
    /// delivery", not "delivered".
    [[nodiscard]] Result<void> submit(ConnectionId id, SessionCommand command);

    /// The same for a handshake decision: runs `ISessionDirectory::decide` on the loop thread.
    [[nodiscard]] Result<void> submit_decision(ConnectionId id, HandshakeVerdict verdict);

    /// After this, submit() fails with Closing. Safe to call more than once.
    void close() noexcept;

private:
    std::mutex mutex_;
    EventLoop* loop_;
    ISessionDirectory* directory_;
};

/// A kept-alive, copyable way to send to one connection from any thread (FR-S3, EC-N13).
/// send_text and close check their arguments (UTF-8, a sendable close code) before queueing
/// anything and return Error{InvalidUtf8} / Error{InvalidArgument} to the caller.
///
/// It holds the connection's id and the loop's link, never a pointer to the connection: a send is
/// copied, posted to the owning loop, and applied there only if the id is still alive. So it is
/// safe to keep after the connection, the loop or the whole server is gone; the call then returns
/// Closing, or the command is silently dropped. A message the connection's own queue refuses is
/// handled by the slow-consumer policy on the loop thread, as for any send (ADR-0013 section 4).
/// Messages from one thread to one connection arrive in order; across threads there is no order.
/// Thread-affinity: any thread.
class SessionHandle {
public:
    SessionHandle() = default;
    SessionHandle(std::shared_ptr<LoopLink> link, ConnectionId id) noexcept
        : link_{std::move(link)}, id_{id} {}

    /// 0 for a default-constructed (inert) handle; connection ids start at 1.
    [[nodiscard]] ConnectionId id() const noexcept {
        return id_;
    }

    [[nodiscard]] Result<void> send_text(std::string_view text) const;
    [[nodiscard]] Result<void> send_binary(ByteView data) const;
    [[nodiscard]] Result<void> close(CloseCode code, std::string_view reason) const;
    /// Lift a pause_reads() on the connection (Session::resume_reads), from any thread.
    [[nodiscard]] Result<void> resume_reads() const;

private:
    std::shared_ptr<LoopLink> link_;
    ConnectionId id_ = 0;
};

}  // namespace wsrv
