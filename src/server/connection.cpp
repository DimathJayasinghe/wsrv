// SPDX-License-Identifier: Apache-2.0
#include "server/connection.hpp"

#include "base/assert.hpp"

#include <algorithm>
#include <exception>
#include <new>
#include <utility>

namespace wsrv {

namespace {

/// ProtocolSession::tick() wants to be called at least once a second: its rate-limit windows are
/// measured in ticks.
constexpr std::chrono::milliseconds kTickInterval{1000};

}  // namespace

// ---- handler barrier ----------------------------------------------------------------------------

/// The one place handler code is called from (FR-S6, EC-A01): whatever it throws, the connection
/// is closed with 1011 and the server carries on. The catch-all is deliberate (user code may throw
/// anything) and never swallows silently: it logs and closes.
template <class Call>
void Connection::guarded(Call&& call) {
    try {
        call();
        return;
    } catch (...) {
        log(LogLevel::Error, "handler threw an exception");
    }
    close(CloseCode::InternalError, "internal error");
}

Connection::Connection(ConnectionId id,
                       const Endpoint& peer,
                       std::unique_ptr<ITransport> transport,
                       const ConnectionContext& context)
    : id_{id}, peer_{peer}, transport_{std::move(transport)}, ctx_{context},
      driver_{context.negotiator, context.config.http} {}

Connection::~Connection() {
    release();  // normally finish() already did; this covers a server torn down with live clients
}

Result<void> Connection::start() {
    const Result<WatchHandle> watch = ctx_.host.watch(transport_->fd(), interest_, *this);
    if (!watch) {
        finish(std::nullopt);
        return std::unexpected{watch.error()};
    }
    watch_ = *watch;
    watching_ = true;
    if (!arm(handshake_timer_, ctx_.config.handshake_timeout, &Connection::on_handshake_timeout)) {
        finish(std::nullopt);
        return fail(ErrorCode::BufferFull, "connection: cannot arm the handshake timer");
    }
    return {};
}

// ---- events from the loop -----------------------------------------------------------------------

// NOLINTNEXTLINE(bugprone-exception-escape): the try block is the loop boundary, see below
void Connection::on_event(PollEvents events) noexcept {
    if (phase_ == Phase::Finished) {
        return;
    }
    try {
        if (phase_ == Phase::AwaitingDecision) {
            on_waiting_event(events);
            return;
        }
        const bool dead_peer = has(events, PollEvents::Hangup) || has(events, PollEvents::Error);
        if (dead_peer) {
            reads_paused_ = false;  // a dead peer cannot be slowed down: drain it and see the end
        }
        if ((has(events, PollEvents::Read) && !reads_paused_) || dead_peer) {
            on_readable();
        }
        if (phase_ != Phase::Finished && has(events, PollEvents::Write)) {
            flush();
            notify_writable();  // after the flush: the queue is as low as it will get this turn
        }
        settle();
    } catch (const std::exception&) {
        // The loop boundary: most likely an allocation failure while serving this one client.
        // Dropping it is the safe answer; the server and its other clients carry on.
        log(LogLevel::Error, "internal error, dropping the connection");
        drop_transport();
    }
}

void Connection::begin_shutdown() {
    if (phase_ == Phase::Handshaking) {
        finish(std::nullopt);
    } else if (phase_ == Phase::AwaitingDecision) {
        refuse(503);  // EC-H32: the client was told "valid, wait"; it gets a real answer
        settle();
    } else if (phase_ == Phase::Established) {
        close(CloseCode::GoingAway, "server shutting down");  // EC-C18
        settle();
    }
}

void Connection::abort() {
    drop_transport();
}

// ---- reading ------------------------------------------------------------------------------------

/// Edge-triggered: read until WouldBlock, but never more than the budget (EC-N07, EC-N08). If the
/// budget runs out, rewatch() re-arms the edge so the kernel reports the remaining data in the next
/// loop iteration, after every other ready descriptor had its turn.
void Connection::on_readable() {
    const ReadBudget& budget = ctx_.config.read;
    std::size_t reads = 0;
    std::size_t bytes = 0;
    while (phase_ != Phase::Finished && phase_ != Phase::AwaitingDecision && !transport_dead_ &&
           !reads_paused_) {
        if (reads >= budget.max_reads_per_event || bytes >= budget.max_bytes_per_event) {
            if (const Result<void> again = ctx_.host.rewatch(watch_, interest_); !again) {
                transport_failed(again.error());
                return;
            }
            schedule_reread();  // a transport that buffers (TLS) is not re-announced by the kernel
            return;
        }
        const Result<std::size_t> got = transport_->read(ctx_.scratch);
        ++reads;
        if (!got) {
            if (got.error().code != ErrorCode::WouldBlock) {
                transport_failed(got.error());
            }
            return;
        }
        if (*got == 0) {
            on_peer_eof();
            return;
        }
        bytes += *got;
        on_bytes(ctx_.scratch.first(*got));
    }
}

void Connection::on_bytes(MutableByteView chunk) {
    if (phase_ == Phase::Handshaking) {
        on_handshake_bytes(chunk);
    } else if (phase_ == Phase::Established) {
        feed_session(chunk);
    }
}

void Connection::on_handshake_bytes(MutableByteView chunk) {
    HandshakeOutcome outcome = driver_.feed(chunk);
    switch (outcome.step) {
    case HandshakeStep::NeedMore:
    case HandshakeStep::Finished:
        return;
    case HandshakeStep::Rejected:
        disarm(handshake_timer_);
        head_out_ = std::move(outcome.response);
        close_after_head_ = true;
        return;
    case HandshakeStep::Accepted: {
        const MutableByteView behind = chunk.subspan(std::min(outcome.consumed, chunk.size()));
        if (ctx_.decider != nullptr) {
            await_decision(outcome, behind);
        } else {
            establish(outcome, behind);
        }
        return;
    }
    }
}

/// The 101 is queued (it is written before any frame), the session starts Open and the handler
/// hears about it. Bytes that arrived behind the request head are the first frame bytes and go
/// straight to the session (EC-H19, FR-H8).
void Connection::establish(HandshakeOutcome& outcome, MutableByteView leftover) {
    disarm(handshake_timer_);
    head_out_ = std::move(outcome.response);
    phase_ = Phase::Established;
    session_.emplace(ctx_.config.session, ctx_.host.now());

    ConnectionInfo info{id_, peer_, std::move(outcome.subprotocol), std::move(outcome.annotations)};
    guarded([&] { handler_ = ctx_.factory.create(info); });
    if (handler_) {
        opened_ = true;  // from here on_close is owed, even if on_open throws
        guarded([&] { handler_->on_open(*this); });
    } else {
        log(LogLevel::Error, "no handler for the connection");
        close(CloseCode::InternalError, "internal error");
    }
    if (!arm(tick_timer_, kTickInterval, &Connection::on_tick)) {
        drop_transport();
        return;
    }
    feed_session(leftover);
}

// ---- the asynchronous handshake decision (ADR-0018) ---------------------------------------------

/// The upgrade is valid, but only the application knows whether this client may come in. Park it:
/// keep what is needed to finish later, stop reading (the client's next bytes stay in the kernel,
/// not in our memory) and ask. Every way out of this function that is not "asked" refuses with a
/// real HTTP answer: the hook never accepts by default.
void Connection::await_decision(HandshakeOutcome& outcome, ByteView leftover) {
    disarm(handshake_timer_);
    std::optional<PendingSlot> slot =
        ctx_.pending != nullptr ? ctx_.pending->try_enter() : std::optional<PendingSlot>{};
    if (!slot) {
        log(LogLevel::Warn, "too many upgrades waiting for a decision, refusing");  // EC-H31
        refuse(503);
        return;
    }
    std::optional<HandshakeRequest> request =
        make_handshake_request(HandshakeFacts{id_, peer_, driver_.request(), outcome.subprotocol},
                               ctx_.config.handshake_forward_headers);
    if (!request) {
        log(LogLevel::Info, "handshake header over the forwarding cap, refusing");  // EC-H34
        refuse(431);
        return;
    }
    auto offered = std::make_shared<const std::vector<std::string>>(request->offered_subprotocols);
    pending_.emplace(PendingUpgrade{std::move(outcome),
                                    std::vector<std::byte>(leftover.begin(), leftover.end()),
                                    std::move(slot),
                                    false});
    phase_ = Phase::AwaitingDecision;
    apply_interest();  // Read off, Hangup on: a client that leaves is noticed at once
    if (phase_ == Phase::Finished) {
        return;  // the registration could not be changed and the connection was dropped
    }
    if (!arm(handshake_timer_,
             ctx_.config.handshake_decision_timeout,
             &Connection::on_decision_timeout)) {
        drop_transport();
        return;
    }
    // From here the decision may arrive at any time, so the state above is complete before asking.
    if (const Result<void> asked =
            ctx_.decider->request(std::move(*request), PendingHandle{ctx_.link, id_, offered});
        !asked) {
        log(LogLevel::Warn, "nobody can decide the handshake, refusing");  // fail closed
        refuse(503);
    } else if (pending_) {
        pending_->asked = true;  // (an answer already delivered inside request() cleared pending_)
    }
}

/// While waiting only the death of the peer matters: nothing is read and nothing is owed.
void Connection::on_waiting_event(PollEvents events) {
    if (has(events, PollEvents::Hangup) || has(events, PollEvents::Error)) {
        finish(std::nullopt);  // EC-H33: no 101 was sent, no handler exists
    }
}

void Connection::decide(const HandshakeVerdict& verdict) {
    if (phase_ != Phase::AwaitingDecision || !pending_) {
        return;  // EC-H30: late, repeated or for a connection that has moved on
    }
    pending_->asked = false;  // answered: the decider has nothing to forget
    if (verdict.accept) {
        accept_pending(verdict);
    } else {
        refuse(is_decision_status(verdict.status) ? verdict.status : 403);
    }
    settle();  // a decision is not inside a callback: write the 101 or the refusal now
}

void Connection::accept_pending(const HandshakeVerdict& verdict) {
    if (!pending_) {
        return;  // decide() checked; this keeps the function safe on its own
    }
    HandshakeOutcome outcome = std::move(pending_->outcome);
    std::vector<std::byte> leftover = std::move(pending_->leftover);
    pending_.reset();  // the place among the waiting upgrades is free from now on
    disarm(handshake_timer_);
    if (verdict.subprotocol && *verdict.subprotocol != outcome.subprotocol) {
        outcome.subprotocol = *verdict.subprotocol;
        outcome.response = render_response(
            HandshakeDecision{HandshakeAccept{outcome.accept_key, outcome.subprotocol, {}, {}}});
    }
    establish(outcome, MutableByteView{leftover});
}

void Connection::end_wait() noexcept {
    if (pending_ && pending_->asked && ctx_.decider != nullptr) {
        pending_->asked = false;
        ctx_.decider->withdrawn(id_);
    }
    pending_.reset();
}

void Connection::refuse(int status) {
    disarm(handshake_timer_);
    end_wait();
    phase_ =
        Phase::Handshaking;  // the driver is finished: later bytes are ignored, as for any refusal
    HandshakeReject reject{status, {}, false};
    if (status == 429 || status == 503) {
        reject.headers.push_back(HttpHeader{"Retry-After", std::to_string(kRetryAfterSeconds)});
    }
    head_out_ = render_response(HandshakeDecision{std::move(reject)});
    close_after_head_ = true;
}

void Connection::on_decision_timeout() {
    handshake_timer_ = TimerHandle{};
    if (phase_ == Phase::AwaitingDecision) {
        log(LogLevel::Warn, "handshake decision timed out, refusing");  // EC-H29
        refuse(503);
        settle();
    }
}

void Connection::feed_session(MutableByteView chunk) {
    ProtocolSession& session = established_session();
    session.tick(ctx_.host.now());
    while (phase_ == Phase::Established) {
        const std::optional<SessionEvent> event = session.next_event(chunk);
        if (!event) {
            return;
        }
        dispatch(*event);
    }
}

void Connection::dispatch(const SessionEvent& event) {
    switch (event.kind) {
    case SessionEvent::Kind::Message:
        guarded([&] {
            if (handler_ == nullptr) {
                return;
            }
            if (event.type == MessageType::Text) {
                handler_->on_text(*this, as_chars(event.data));
            } else {
                handler_->on_binary(*this, event.data);
            }
        });
        return;
    case SessionEvent::Kind::Part:
        deliver_part(event);
        return;
    case SessionEvent::Kind::Ping:  // the Pong is already queued by the session
    case SessionEvent::Kind::Pong:
        return;
    case SessionEvent::Kind::ProtocolError:
        log(LogLevel::Info, "peer broke the protocol");  // the close code travels in CloseInfo
        return;
    case SessionEvent::Kind::Closed:
        finish(CloseInfo{event.code, std::string{event.reason}, event.clean});
        return;
    }
}

/// A streaming part may come as two pieces (a character completed by the next chunk plus the rest
/// of it); both are code-point aligned, so they are delivered as consecutive parts.
void Connection::deliver_part(const SessionEvent& event) {
    guarded([&] {
        if (handler_ == nullptr) {
            return;
        }
        const bool has_tail = !event.tail.empty();
        handler_->on_message_part(*this, event.type, event.data, event.final && !has_tail);
        if (has_tail) {
            handler_->on_message_part(*this, event.type, event.tail, event.final);
        }
    });
}

ProtocolSession& Connection::established_session() {
    WSRV_ASSERT(session_.has_value());  // callers run only in the Established phase
    return *session_;
}

void Connection::on_peer_eof() {
    if (phase_ == Phase::Handshaking) {
        finish(std::nullopt);  // EC-H21: nothing was opened, nothing is owed
        return;
    }
    drop_transport();  // EC-N04: the session reports an unclean close (1006)
}

// ---- writing ------------------------------------------------------------------------------------

/// Write what is queued, then deliver the events that writing caused (a Close that is now fully
/// sent makes the session Closed). Repeats while delivering events produced more output.
void Connection::settle() {
    do {
        flush();
    } while (phase_ != Phase::Finished && drain_events());
    if (phase_ != Phase::Finished) {
        apply_interest();  // Write interest follows blocked output and a pending on_writable
    }
}

/// Collect events that do not need input (Closed after the last byte was written, a timer's
/// ProtocolError...). @return true if there was any.
bool Connection::drain_events() {
    bool any = false;
    MutableByteView nothing;
    while (session_ && phase_ == Phase::Established) {
        const std::optional<SessionEvent> event = session_->next_event(nothing);
        if (!event) {
            break;
        }
        any = true;
        dispatch(*event);
    }
    return any;
}

/// Write one chunk of whatever is queued. Done = nothing left; Progress = bytes moved, call again;
/// Stopped = the kernel is full or the transport failed (both already handled).
Connection::WriteStep Connection::write_once() {
    const ByteView pending = next_output();
    if (pending.empty()) {
        return push_held_output();
    }
    const Result<std::size_t> sent = transport_->write(pending);
    if (!sent || *sent == 0) {
        if (!sent && sent.error().code != ErrorCode::WouldBlock) {
            transport_failed(sent.error());
        } else {
            set_write_blocked(true);
        }
        return WriteStep::Stopped;
    }
    consume_output(*sent);
    note_write_progress();
    return WriteStep::Progress;
}

/// Nothing of ours is queued, but the transport may still hold bytes the descriptor refused (a TLS
/// handshake flight, records not yet sent). Offer it the chance to push them out; while it still
/// cannot, the connection is write-blocked: it asks for "writable" and the stall timer runs.
Connection::WriteStep Connection::push_held_output() {
    if (!transport_->wants_write()) {
        return WriteStep::Done;
    }
    const Result<std::size_t> pushed = transport_->write(ByteView{});
    if (!pushed && pushed.error().code != ErrorCode::WouldBlock) {
        transport_failed(pushed.error());
        return WriteStep::Stopped;
    }
    if (transport_->wants_write()) {
        set_write_blocked(true);
        return WriteStep::Stopped;
    }
    return WriteStep::Done;
}

void Connection::flush() {
    WriteStep step = WriteStep::Progress;
    while (step == WriteStep::Progress && phase_ != Phase::Finished && !transport_dead_) {
        step = write_once();
    }
    if (step == WriteStep::Stopped || phase_ == Phase::Finished || transport_dead_) {
        return;
    }
    set_write_blocked(false);
    if (close_after_head_ && head_sent_ >= head_out_.size()) {
        finish(std::nullopt);  // the refusal has been delivered
    }
}

ByteView Connection::next_output() {
    if (head_sent_ < head_out_.size()) {
        return as_bytes(std::string_view{head_out_}.substr(head_sent_));
    }
    return session_ ? session_->pending_output() : ByteView{};
}

void Connection::consume_output(std::size_t count) {
    if (head_sent_ < head_out_.size()) {
        head_sent_ += count;
    } else if (session_) {
        session_->consume_output(count);
    }
}

/// The stall timer runs exactly as long as the kernel keeps refusing our bytes (EC-N09, T3).
void Connection::set_write_blocked(bool blocked) {
    if (blocked == write_blocked_) {
        return;
    }
    write_blocked_ = blocked;
    apply_interest();
    if (transport_dead_) {
        return;  // re-arming the watch failed and the connection is already being dropped
    }
    if (blocked) {
        if (!arm(stall_timer_, ctx_.config.write_stall_timeout, &Connection::on_write_stall)) {
            drop_transport();
        }
    } else {
        disarm(stall_timer_);
    }
}

/// Write interest costs wakeups, so it is armed only while output is waiting (EC-N02) or an
/// on_writable notification is due: the Write event is what gives that notification its own loop
/// iteration (EC-N08, ADR-0015 section 4). Re-arming an edge-triggered registration makes the
/// kernel report a writable socket again.
void Connection::apply_interest() {
    const bool want_write = write_blocked_ || writable_owed_;
    PollEvents reading = reads_paused_ ? PollEvents::None : PollEvents::Read;
    if (phase_ == Phase::AwaitingDecision) {
        reading = PollEvents::Hangup;  // not Read: unread bytes stay in the kernel, a FIN is seen
    }
    const PollEvents wanted = want_write ? (reading | PollEvents::Write) : reading;
    if (wanted == interest_) {
        return;
    }
    const bool gains_read = !has(interest_, PollEvents::Read) && has(wanted, PollEvents::Read);
    interest_ = wanted;
    if (watching_) {
        if (const Result<void> changed = ctx_.host.rewatch(watch_, interest_); !changed) {
            transport_failed(changed.error());
            return;
        }
        if (gains_read) {
            schedule_reread();  // input a buffering transport already holds gets no new event
        }
    }
}

void Connection::pause_reads() {
    if (reads_paused_ || phase_ == Phase::Finished || transport_dead_) {
        return;
    }
    reads_paused_ = true;
    apply_interest();  // at once: the next epoll_wait must not wake us for input we will not read
}

/// The pause removed Read from the registration, so putting it back is a change of mask and the
/// kernel re-evaluates readiness: data that arrived meanwhile is reported (edge-triggered re-arm).
void Connection::resume_reads() {
    if (!reads_paused_) {
        return;
    }
    reads_paused_ = false;
    if (phase_ != Phase::Finished && !transport_dead_) {
        apply_interest();
    }
}

/// Deliver the notification owed after a refusal, once the queue is at or below the low-water mark
/// (half the limit). The exception barrier and the "not while closing" rule are the usual ones.
void Connection::notify_writable() {
    if (!writable_owed_ || phase_ != Phase::Established || !session_ || handler_ == nullptr) {
        return;
    }
    if (session_->state() != SessionState::Open) {
        writable_owed_ = false;  // nothing more will be sent: do not wake the handler for it
        return;
    }
    const std::size_t low_water = ctx_.config.session.outbound.max_queued_bytes / 2;
    if (session_->queued_output_bytes() > low_water) {
        return;
    }
    writable_owed_ = false;
    // Disarm first: if the handler is refused again, settle() re-arms, and only a fresh arming
    // makes an edge-triggered registration report a writable socket once more.
    apply_interest();
    if (phase_ == Phase::Established) {
        guarded([&] { handler_->on_writable(*this); });
    }
}

/// Bytes moved while the peer is slow but alive: restart the stall clock.
void Connection::note_write_progress() {
    if (write_blocked_ && stall_timer_.id != 0) {
        disarm(stall_timer_);
        if (!arm(stall_timer_, ctx_.config.write_stall_timeout, &Connection::on_write_stall)) {
            drop_transport();
        }
    }
}

Result<void> Connection::apply_send_policy(Result<void> result) {
    if (result || result.error().code != ErrorCode::Backpressure) {
        return result;
    }
    if (ctx_.config.slow_consumer == SlowConsumerPolicy::Close) {
        log(LogLevel::Warn, "slow consumer, closing");
        close(CloseCode::PolicyViolation, "slow consumer");  // the Close has its own queue slot
    } else {
        writable_owed_ = true;  // the handler may retry once the queue has drained
    }
    return result;
}

// ---- Session (what the handler calls) -----------------------------------------------------------

void Connection::apply(const SessionCommand& command) {
    if (phase_ != Phase::Established || !session_) {
        return;
    }
    command(*this);
    settle();
}

Result<void> Connection::send_text(std::string_view text) {
    if (phase_ != Phase::Established || !session_) {
        return fail(ErrorCode::Closing, "connection closed");
    }
    return apply_send_policy(session_->send_text(text));
}

Result<void> Connection::send_binary(ByteView data) {
    if (phase_ != Phase::Established || !session_) {
        return fail(ErrorCode::Closing, "connection closed");
    }
    return apply_send_policy(session_->send_binary(data));
}

// ---- streaming writer ---------------------------------------------------------------------------

Result<MessageWriter> Connection::begin_text() {
    return begin_message(Opcode::Text);
}

Result<MessageWriter> Connection::begin_binary() {
    return begin_message(Opcode::Binary);
}

Result<MessageWriter> Connection::begin_message(Opcode type) {
    if (phase_ != Phase::Established || !session_) {
        return fail(ErrorCode::Closing, "connection closed");
    }
    if (stream_link_ == nullptr) {
        try {
            stream_link_ = std::make_shared<StreamLink>();
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::ResourceExhausted, "connection: out of memory");
        }
        stream_link_->sink = this;
    }
    if (const Result<void> opened = session_->begin_message(type); !opened) {
        return std::unexpected{opened.error()};
    }
    return MessageWriter{stream_link_};
}

/// Backpressure here is a signal, not a fault: the writer is cooperative, so the slow-consumer
/// policy does not fire (ADR-0015 section 5); the stall timer still bounds a peer that reads
/// nothing.
Result<void> Connection::stream_write(ByteView bytes) {
    if (phase_ != Phase::Established || !session_) {
        return fail(ErrorCode::Closing, "connection closed");
    }
    Result<void> written = session_->write_part(bytes);
    if (!written && written.error().code == ErrorCode::Backpressure) {
        writable_owed_ = true;
    }
    return written;
}

Result<void> Connection::stream_finish() {
    if (phase_ != Phase::Established || !session_) {
        return fail(ErrorCode::Closing, "connection closed");
    }
    return session_->end_message();
}

/// A writer was destroyed with its message open (EC-F34). If the message is already gone because
/// the session is closing, there is nothing to blame on the handler.
void Connection::stream_abandon() noexcept {
    if (phase_ != Phase::Established || !session_ || !session_->message_open()) {
        return;
    }
    log(LogLevel::Warn, "handler dropped an unfinished message, closing");
    close(CloseCode::InternalError, "unfinished message");
}

/// Only records the request: the Close frame is queued and written when the current callback has
/// returned and the connection settles (EC-A06).
void Connection::close(CloseCode code, std::string_view reason) {
    if (phase_ != Phase::Established || !session_) {
        return;
    }
    if (session_->close(code, reason)) {
        return;
    }
    log(LogLevel::Warn, "handler asked for a close code that cannot be sent");
    if (!session_->close(CloseCode::InternalError, "internal error")) {
        log(LogLevel::Error, "cannot close with 1011");  // 1011 is always sendable: unreachable
    }
}

// ---- ending -------------------------------------------------------------------------------------

void Connection::transport_failed(const Error& error) {
    if (error.code == ErrorCode::Io && opened_ && handler_) {
        guarded([&] { handler_->on_error(*this, error); });
    }
    drop_transport();
}

/// The byte stream is gone (reset, EOF, write failure, stall, shutdown deadline). The session
/// reports an unclean Closed at once; nothing more is written.
void Connection::drop_transport() {
    transport_dead_ = true;
    if (session_ && phase_ == Phase::Established) {
        session_->on_transport_closed();
        static_cast<void>(drain_events());
    }
    finish(std::nullopt);
}

/// Release the descriptor, tell the handler (once, only if it was opened), then ask the owner to
/// destroy us at a safe point. Never destroys `this` itself (ADR-0013).
void Connection::finish(const std::optional<CloseInfo>& info) {
    if (phase_ == Phase::Finished) {
        return;
    }
    phase_ = Phase::Finished;
    release();
    if (opened_) {
        opened_ = false;
        const CloseInfo reported = info.value_or(CloseInfo{});
        guarded([&] { handler_->on_close(*this, reported); });
    }
    ctx_.host.retire(id_);
}

void Connection::release() noexcept {
    if (released_) {
        return;
    }
    released_ = true;
    if (stream_link_ != nullptr) {
        stream_link_->sink = nullptr;  // writers that outlive us now fail with Closing
    }
    disarm(handshake_timer_);
    disarm(tick_timer_);
    disarm(stall_timer_);
    disarm(reread_timer_);
    end_wait();  // frees the place among the waiting upgrades, whatever ended the wait
    if (watching_) {
        ctx_.host.unwatch(watch_);
        watching_ = false;
    }
    if (!transport_dead_) {
        transport_->shutdown_write();  // a clean FIN after our last byte
    }
    transport_->close();
}

// ---- timers -------------------------------------------------------------------------------------

bool Connection::arm(TimerHandle& slot,
                     std::chrono::milliseconds delay,
                     void (Connection::*action)()) {
    const Result<TimerHandle> timer =
        ctx_.host.run_after(delay, [this, action] { (this->*action)(); });
    if (!timer) {
        slot = TimerHandle{};
        log(LogLevel::Error, "cannot arm a timer");
        return false;
    }
    slot = *timer;
    return true;
}

void Connection::disarm(TimerHandle& slot) noexcept {
    if (slot.id != 0) {
        ctx_.host.cancel(slot);
        slot = TimerHandle{};
    }
}

void Connection::on_handshake_timeout() {
    handshake_timer_ = TimerHandle{};
    if (phase_ == Phase::Handshaking) {
        log(LogLevel::Info, "handshake timeout");  // EC-H20: no response is owed
        finish(std::nullopt);
    }
}

void Connection::on_tick() {
    tick_timer_ = TimerHandle{};
    if (phase_ != Phase::Established) {
        return;
    }
    established_session().tick(ctx_.host.now());
    settle();
    if (phase_ == Phase::Established && !arm(tick_timer_, kTickInterval, &Connection::on_tick)) {
        drop_transport();
    }
}

/// A transport that buffers (TLS) may hold input the kernel will never announce again; when the
/// connection stopped or paused reading it must come back for it by itself, on a later turn of the
/// loop so other connections are served in between.
void Connection::schedule_reread() {
    if (reread_timer_.id != 0 || phase_ == Phase::Finished || transport_dead_ ||
        !transport_->buffered_input()) {
        return;
    }
    if (!arm(reread_timer_, std::chrono::milliseconds{0}, &Connection::on_reread)) {
        drop_transport();
    }
}

void Connection::on_reread() {
    reread_timer_ = TimerHandle{};
    on_event(PollEvents::Read);  // the same path (and exception barrier) as a real readiness event
}

void Connection::on_write_stall() {
    stall_timer_ = TimerHandle{};
    if (phase_ != Phase::Finished) {
        log(LogLevel::Warn, "peer accepts no data, dropping the connection");
        drop_transport();
    }
}

void Connection::log(LogLevel level, std::string_view message) const {
    ctx_.host.log(level, id_, message);
}

}  // namespace wsrv
