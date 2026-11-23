// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/error.hpp"
#include "net/fd.hpp"
#include "net/syscalls.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

#include "capi/host_event.hpp"

namespace wsrv {

/// Both limits apply; neither can be exceeded, whatever the number of connections or the size of
/// the messages (ADR-0016 section 4).
struct EventQueueConfig {
    std::size_t max_events = 4096;
    std::size_t max_bytes = std::size_t{8} << 20;
};

/// What a producer learns from a successful push.
struct PushOutcome {
    /// The queue is at or above high water (3/4 of the room left after the Close slots): the
    /// producing connection should stop reading until it is told to resume.
    bool pause_reads = false;
};

/// What the consumer learns from a successful pop.
struct Popped {
    HostEvent event;
    /// The queue just fell to low water (1/2 of the room left) after having been at high water:
    /// paused connections may resume. True for exactly one pop per pressure episode; the pop that
    /// empties the queue always qualifies.
    bool resume_reads = false;
};

/// The one hand-over point between the loop threads that produce events and the host thread that
/// consumes them (ADR-0016 section 3). Many producers, one consumer, bounded in events and bytes.
///
/// Opening a connection reserves room for its Close event, so a Close can never be refused: the
/// host sees every Open it was told about end with exactly one Close, and can free its state.
/// Everything else is refused with BufferFull when it does not fit, and counted in dropped().
///
/// event_fd() is readable exactly while an event is pending. The descriptor is written by the push
/// that makes the queue non-empty and drained by the pop that empties it, both under the queue's
/// mutex, so a wake-up cannot be lost between a producer's push and the consumer's sleep (lesson
/// 13). The mutex lives here, at the edge, and is held for one push or pop; Connection and
/// ProtocolSession stay lock-free.
/// Thread-affinity: push() any thread; try_pop() one consumer thread at a time; the rest any.
class EventQueue {
public:
    /// @return Error from the kernel (no more descriptors) or ResourceExhausted.
    [[nodiscard]] static Result<std::unique_ptr<EventQueue>> create(ISyscalls& sys,
                                                                    EventQueueConfig config);

    /// Construction goes through create() (passkey, so make_unique can call it).
    class Key {
        friend class EventQueue;
        Key() = default;
    };
    EventQueue(Key /*unused*/, ISyscalls& sys, EventQueueConfig config, Fd wake) noexcept;

    EventQueue(const EventQueue&) = delete;
    EventQueue& operator=(const EventQueue&) = delete;
    EventQueue(EventQueue&&) = delete;
    EventQueue& operator=(EventQueue&&) = delete;
    ~EventQueue() = default;

    /// Queue `event`; nothing is queued on failure.
    /// - Open needs room for itself and for the Close it reserves.
    /// - Close needs an accepted Open (else InvalidArgument) and always fits.
    /// - Text, Binary and Error need room for themselves and leave the reservations alone.
    /// @return Error{BufferFull} over a limit, Error{ResourceExhausted}, Error{Io}.
    ///
    /// `wake` says when a push that finds the queue empty wakes the host (ADR-0035): Now writes the
    /// descriptor inside push(); AtFlush only owes the wake-up, and flush_wake() pays it. A loop
    /// that handles many ready connections in one iteration pushes many events: one wake-up for the
    /// whole burst, not one per event, which on a busy server saves the host a context switch per
    /// message. Use AtFlush only from a loop thread that calls flush_wake() at the end of its
    /// iteration; every other push (and every rare event) uses Now.
    enum class Wake : std::uint8_t { Now, AtFlush };
    [[nodiscard]] Result<PushOutcome> push(HostEvent event, Wake wake = Wake::Now);

    /// Pay the wake-up that a Wake::AtFlush push owes, if any (a no-op, without taking the lock,
    /// when none is owed). Called by the thread that pushed, after its burst. A host that emptied
    /// the queue first has already cancelled the wake-up, and nothing is written.
    void flush_wake();

    /// @return The oldest event, or nothing when the queue is empty. Never blocks: wait on
    /// event_fd() and call again.
    [[nodiscard]] std::optional<Popped> try_pop();

    /// Valid for the queue's lifetime; the host may add it to its own poll set.
    [[nodiscard]] int event_fd() const noexcept {
        return wake_.get();
    }

    /// True between reaching high water and the pop that reports the resume. A producer that has
    /// just paused its connection asks this to close the race with a consumer that drained the
    /// queue in between (see HostBridge).
    [[nodiscard]] bool pressured() const;

    /// Events waiting right now. A snapshot: it may be stale when the caller looks at it.
    [[nodiscard]] std::size_t pending() const;

    /// Events refused because a limit was reached, since the queue was created.
    [[nodiscard]] std::uint64_t dropped() const noexcept {
        return dropped_.load();
    }

    /// Bytes set aside for each Close; the close reason is cut to fit (RFC 6455 section 5.5).
    static constexpr std::size_t kReservedCloseBytes = kMaxCloseReason;

private:
    [[nodiscard]] Result<void> admit(const HostEvent& event);
    [[nodiscard]] bool fits(std::size_t events, std::size_t bytes) const noexcept;
    [[nodiscard]] bool at_high_water() const noexcept;
    [[nodiscard]] bool at_low_water() const noexcept;

    ISyscalls& sys_;
    const EventQueueConfig config_;
    Fd wake_;
    std::atomic<std::uint64_t> dropped_{0};
    /// Mirrors wake_owed_ so flush_wake() can return without the lock. Only an optimisation: the
    /// thread that pushed sees its own store, so it never skips its own wake-up.
    std::atomic<bool> owed_hint_{false};

    mutable std::mutex mutex_;
    std::deque<HostEvent> events_;     // guarded by mutex_
    bool wake_owed_ = false;           // guarded: a Wake::AtFlush push is waiting for flush_wake()
    std::size_t bytes_ = 0;            // guarded: payload bytes of events_
    std::size_t reserved_closes_ = 0;  // guarded: accepted Opens whose Close is still to come
    std::size_t reserved_bytes_ = 0;   // guarded: reserved_closes_ * kReservedCloseBytes
    bool pressured_ = false;           // guarded: high water was reached and not yet left
};

}  // namespace wsrv
