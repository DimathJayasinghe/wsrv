// SPDX-License-Identifier: Apache-2.0
#include "server/connection.hpp"

#include "base/assert.hpp"

#include <algorithm>
#include <exception>
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
        if (has(events, PollEvents::Read) || has(events, PollEvents::Hangup) ||
            has(events, PollEvents::Error)) {
            on_readable();
        }
        if (phase_ != Phase::Finished && has(events, PollEvents::Write)) {
            flush();
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
    while (phase_ != Phase::Finished && !transport_dead_) {
        if (reads >= budget.max_reads_per_event || bytes >= budget.max_bytes_per_event) {
            if (const Result<void> again = ctx_.host.rewatch(watch_, interest_); !again) {
                transport_failed(again.error());
            }
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
    case HandshakeStep::Accepted:
        establish(outcome, chunk.subspan(std::min(outcome.consumed, chunk.size())));
        return;
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
        return WriteStep::Done;
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

/// Write interest costs wakeups, so it is armed only while output is waiting (EC-N02). The stall
/// timer runs exactly as long as the kernel keeps refusing our bytes (EC-N09, T3).
void Connection::set_write_blocked(bool blocked) {
    if (blocked == write_blocked_) {
        return;
    }
    write_blocked_ = blocked;
    interest_ = blocked ? (PollEvents::Read | PollEvents::Write) : PollEvents::Read;
    if (watching_) {
        if (const Result<void> changed = ctx_.host.rewatch(watch_, interest_); !changed) {
            transport_failed(changed.error());
            return;
        }
    }
    if (blocked) {
        if (!arm(stall_timer_, ctx_.config.write_stall_timeout, &Connection::on_write_stall)) {
            drop_transport();
        }
    } else {
        disarm(stall_timer_);
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
    if (!result && result.error().code == ErrorCode::Backpressure &&
        ctx_.config.slow_consumer == SlowConsumerPolicy::Close) {
        log(LogLevel::Warn, "slow consumer, closing");
        close(CloseCode::PolicyViolation, "slow consumer");  // the Close has its own queue slot
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
    disarm(handshake_timer_);
    disarm(tick_timer_);
    disarm(stall_timer_);
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
