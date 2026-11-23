// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/bytes.hpp"
#include "base/error.hpp"
#include "server/broker.hpp"
#include "server/handler.hpp"
#include "server/handshake_decider.hpp"
#include "server/session_handle.hpp"

#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "capi/event_queue.hpp"

namespace wsrv {

/// The embedding seam (ADR-0016): turns what happens on the loop threads into events a host
/// thread can pull, and lets the host act on connections by id from its own thread.
///
/// It is the whole of "what a binding needs from the core", written once in C++ so that every
/// binding (PHP, C#, Java, Node...) is only a translation of types: the socket and state edge
/// cases are handled here and below, never in a binding.
///   - Every Open is followed by exactly one Close, so the host can always free what it kept.
///   - Events of one connection arrive in order: Open, messages, Close.
///   - A host that stops polling cannot grow memory: past the queue limit a connection is closed
///     with 1013 (TryAgainLater) and its events are dropped, never the Close.
///   - A send to a connection that is gone, or never existed, fails with Closing; it never
///     touches freed memory, whatever the timing (EC-N13).
/// Plug it in as the server's IHandlerFactory; poll events() from the host.
///   - A host that falls behind slows its clients instead of losing their messages: past high
///     water the producing connection stops reading (TCP flow control does the rest) and resumes
///     when poll() has drained the queue to low water.
/// Thread-affinity: the factory part on loop threads; send_*, close, events() any thread, with a
/// single consumer thread for poll().
class HostBridge final : public IHandlerFactory, public IHandshakeDecider {
public:
    /// @return Error from EventQueue::create.
    /// `broker` (optional, ADR-0027) is the pub/sub broker the host drives by connection id;
    /// without one every broker command is Error{InvalidArgument}.
    [[nodiscard]] static Result<std::unique_ptr<HostBridge>>
    make(ISyscalls& sys, EventQueueConfig queue, std::shared_ptr<Broker> broker = nullptr);

    /// Construction goes through make() (passkey, so make_unique can call it).
    class Key {
        friend class HostBridge;
        Key() = default;
    };
    HostBridge(Key /*unused*/,
               std::unique_ptr<EventQueue> events,
               std::shared_ptr<Broker> broker) noexcept
        : events_{std::move(events)}, broker_{std::move(broker)} {}

    [[nodiscard]] std::unique_ptr<IWebSocketHandler> create(const ConnectionInfo& info) override;

    /// IHandshakeDecider: queue a Handshake event for the host and remember how to answer it. A
    /// full queue is an Error (the server then refuses the upgrade with 503, never accepts).
    [[nodiscard]] Result<void> request(HandshakeRequest request, PendingHandle handle) override;
    void withdrawn(ConnectionId id) noexcept override;

    /// Answer the waiting upgrade `id` from a Handshake event. The answer is consumed: a second
    /// one, or one for an upgrade the server gave up on (timeout, client left), is Closing.
    /// @return Error{Closing} (unknown, answered or ended), Error{InvalidArgument} (a subprotocol
    /// the client did not offer, a status outside kDecisionStatuses; the upgrade keeps waiting),
    /// Error{BufferFull} (the loop's queue is full; try again).
    [[nodiscard]] Result<void> accept_handshake(ConnectionId id,
                                                std::optional<std::string_view> subprotocol);
    [[nodiscard]] Result<void> reject_handshake(ConnectionId id, int status);

    /// Open or close the door for questions. While it is closed (nobody is there to answer, as in
    /// wsrvd without an application), request() fails at once, so the server refuses the upgrade
    /// with 503 and nothing is queued. Closing it also refuses, with 503, every upgrade that is
    /// waiting: its question went to a host that is gone. Open by default. Any thread.
    void set_handshake_intake(bool open);

    /// How many upgrades wait for an answer from the host right now (a gauge for tests and, later,
    /// metrics). Never counts one the server already gave up on.
    [[nodiscard]] std::size_t waiting_handshakes();

    /// The next event for the host, or nothing when none is waiting. Never blocks: wait on
    /// events().event_fd() and call again. Draining the queue is what lets connections that were
    /// paused for being too fast resume reading, so the host must consume events through this
    /// and not through events().try_pop(). One consumer thread at a time.
    [[nodiscard]] std::optional<HostEvent> poll();

    /// The queue behind poll(): its descriptor and counters. Valid for the bridge's lifetime.
    [[nodiscard]] EventQueue& events() noexcept {
        return *events_;
    }

    /// Queue a message for connection `id`; "accepted" means handed to its loop, not delivered
    /// (SessionHandle). Text is checked as UTF-8 here, so the host learns of a bad string at once.
    /// @return Error{Closing} for an unknown or finished connection, Error{InvalidUtf8},
    /// Error{BufferFull}, Error{ResourceExhausted}.
    [[nodiscard]] Result<void> send_text(ConnectionId id, std::string_view text);
    [[nodiscard]] Result<void> send_binary(ConnectionId id, ByteView data);
    /// Start the closing handshake of connection `id` (same errors as send_text).
    [[nodiscard]] Result<void> close(ConnectionId id, CloseCode code, std::string_view reason);

    /// The broker by connection id (ADR-0027). The bridge removes a connection's subscriptions when
    /// it closes, so the host cannot leak one. Lock order: this bridge's mutex, then the broker's;
    /// the broker never calls back into the bridge. subscribe() holds the bridge mutex across the
    /// broker call so that it cannot interleave with remove(): either the subscription is complete
    /// before the close cleans up, or the connection is not found. Never held while sending.
    /// @return Error{InvalidArgument} (no broker, or a bad topic name), Error{Closing} (unknown or
    /// finished connection), Error{BufferFull} (a broker bound), Error{ResourceExhausted}.
    [[nodiscard]] Result<void> subscribe(ConnectionId id, std::string_view topic);
    [[nodiscard]] Result<void> unsubscribe(ConnectionId id, std::string_view topic);
    /// Send to every subscriber of `topic` except `except` (0 = nobody). Errors as
    /// Broker::publish_* plus Error{InvalidArgument} when there is no broker. Any thread.
    [[nodiscard]] Result<PublishReport>
    publish_text(std::string_view topic, std::string_view text, ConnectionId except = 0);
    [[nodiscard]] Result<PublishReport>
    publish_binary(std::string_view topic, ByteView data, ConnectionId except = 0);
    /// Remove every subscription of `id` (idempotent; a no-op without a broker). For a host that
    /// stopped serving the connection but cannot close it yet (wsrvd, ADR-0028 A1).
    void drop_subscriptions(ConnectionId id) noexcept {
        if (broker_) {
            broker_->unsubscribe_all(id);
        }
    }
    /// Report the broker's counters to `metrics` (the server's shared metrics); no-op without a
    /// broker.
    void use_broker_metrics(std::shared_ptr<IMetrics> metrics) noexcept {
        if (broker_) {
            broker_->use_metrics(std::move(metrics));
        }
    }
    /// 0 when there is no broker or no such topic.
    [[nodiscard]] std::size_t topic_subscribers(std::string_view topic) const;

private:
    class Handler;

    void add(ConnectionId id, SessionHandle handle);
    void remove(ConnectionId id);
    void park(ConnectionId id);
    bool unpark(ConnectionId id);
    void resume_parked();
    [[nodiscard]] std::optional<SessionHandle> find(ConnectionId id);
    [[nodiscard]] std::optional<PendingHandle> find_asked(ConnectionId id);
    void forget_asked(ConnectionId id);

    const std::unique_ptr<EventQueue> events_;
    const std::shared_ptr<Broker> broker_;  // null: no pub/sub; Broker has its own mutex
    std::mutex mutex_;  // guards handles_; held for one map operation, never across a call out
    std::unordered_map<ConnectionId, SessionHandle> handles_;
    std::unordered_set<ConnectionId> parked_;  // paused for a full queue; guarded by mutex_
    std::unordered_map<ConnectionId, PendingHandle> asked_;  // upgrades awaiting the host; mutex_
    bool intake_open_ = true;                                // guarded by mutex_
};

}  // namespace wsrv
