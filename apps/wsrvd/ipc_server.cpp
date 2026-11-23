// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/ipc_server.hpp"

#include "base/byte_buffer.hpp"
#include "net/fd.hpp"
#include "net/plain_transport.hpp"
#include "wsrv/wsrv.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <sys/socket.h>

#include "ipc/frame_decoder.hpp"
#include "ipc/message.hpp"
#include "wsrvd/event_convert.hpp"

namespace wsrv::wsrvd {

namespace {

constexpr int kBacklog = 4;  // one consumer: a handful of waiting connects is plenty
constexpr std::size_t kMaxAcceptsPerEvent = 16;
constexpr std::size_t kMaxReadsPerEvent = 16;   // fairness budget (EC-N07, EC-N08)
constexpr std::size_t kMaxEventsPerFill = 256;  // events encoded before the buffer is flushed
constexpr std::size_t kMaxPumpRounds = 16;      // fill+flush rounds before the loop gets a turn
constexpr std::chrono::milliseconds kAcceptRetry{100};
/// How soon a stale close that a full loop queue refused is tried again (ADR-0034).
constexpr std::chrono::milliseconds kCloseRetry{25};
// Close codes (RFC 6455 section 7.4.1; 1012 is in the IANA registry): 1001 Going Away,
// 1012 Service Restart.
constexpr std::uint16_t kCloseGoingAway = 1001;
constexpr std::uint16_t kCloseServiceRestart = 1012;
/// Room above the two frames the pump may add, for the answers to one read's worth of commands.
/// The smallest command is 18 bytes and its STATUS 11, and a PING is 6 bytes and its PONG 6, so
/// one 16 KiB read can produce at most this much (the read loop checks the high-water mark first).
constexpr std::size_t kAnswerReserve = 16384;
constexpr int kStatusUnavailable = 503;

}  // namespace

// ---- one connected application ------------------------------------------------------------------

/// The state of one IPC connection: bytes in, decoder, handshake, bytes out. Everything runs on the
/// loop thread. A Peer never destroys itself from inside its own callback: close() only marks it
/// closed and asks the server to release it at the end of the loop iteration (ADR-0013).
///
/// Output is bounded. `out_` holds at most high water (one max frame) plus the last frame the pump
/// added plus the answers to one read; above high water the pump stops taking events from the
/// backend (so the event queue's own high-water mark pauses the WebSocket clients) and the peer
/// stops reading commands (so it cannot grow `out_` with answers). The one thing that ends this
/// state is the application reading, or the stall timer closing the link.
class IpcServer::Peer final : public IEventHandler {
public:
    Peer(IpcServer& server, Fd fd)
        : server_{server}, transport_{std::move(fd), server.sys_},
          decoder_{std::make_unique<ipc::FrameDecoder>(kHelloMaxFrame)},
          high_water_{server.config_.max_frame},
          out_{2 * static_cast<std::size_t>(server.config_.max_frame) + kAnswerReserve} {}
    Peer(const Peer&) = delete;
    Peer& operator=(const Peer&) = delete;
    Peer(Peer&&) = delete;
    Peer& operator=(Peer&&) = delete;
    ~Peer() override {
        teardown();
    }

    [[nodiscard]] bool closed() const noexcept {
        return closed_;
    }

    /// Start watching the socket and the HELLO deadline.
    [[nodiscard]] Result<void> start() {
        const Result<WatchHandle> watch =
            server_.loop_.watch(transport_.fd(), PollEvents::Read, *this);
        if (!watch) {
            return std::unexpected{watch.error()};
        }
        watch_ = *watch;
        const Result<TimerHandle> timer =
            server_.loop_.run_after(server_.config_.hello_timeout, [this] {
                hello_timer_.reset();
                if (state_ == State::AwaitHello) {
                    close();  // a peer that stays silent must not hold the only slot
                }
            });
        if (!timer) {
            return std::unexpected{timer.error()};
        }
        hello_timer_ = *timer;
        return {};
    }

    void on_event(PollEvents events) noexcept override {
        if (closed_) {
            return;
        }
        if (has(events, PollEvents::Write)) {
            if (flush()) {
                pump();  // room again: take more events from the backend
            }
        }
        if (closed_) {
            return;
        }
        const bool gone = has(events, PollEvents::Hangup) || has(events, PollEvents::Error);
        if (reads_paused()) {
            // Reading is off, so a hang-up would never be noticed through a read. Nobody is left
            // to read what we hold, so drop the link.
            if (gone) {
                close();
            }
            return;
        }
        if (has(events, PollEvents::Read) || gone) {
            on_readable();
        }
    }

    /// Look at the socket now instead of waiting for the loop to report it: used before refusing a
    /// newcomer, so a peer that has just gone away does not keep the slot for one more iteration.
    void look_now() noexcept {
        if (!closed_) {
            on_readable();
        }
    }

    /// Move events from the backend into the output buffer, and the buffer into the socket, until
    /// there are no more events, the buffer is full, or this turn's budget is spent.
    ///
    /// Re-entry is refused: flush() may call close() and the loop may call pump() again from
    /// inside a callback, but a pump is never inside a pump.
    void pump() noexcept {
        if (state_ != State::Ready || closed_ || closing_after_flush_ || pumping_) {
            return;
        }
        pumping_ = true;
        for (std::size_t round = 0; !closed_; ++round) {
            const Fill fill = fill_output();
            if (closed_ || !flush() || fill == Fill::Empty) {
                // Closed; or the socket is full (the Write event will call pump() again once it
                // drains); or the backend had nothing more (its next event will wake us).
                break;
            }
            if (round + 1 >= kMaxPumpRounds) {
                // Events remain and the socket takes them, but other work deserves a turn.
                // The backend's wake-up is edge-triggered: ask for a fresh edge.
                server_.rearm_event_source();
                break;
            }
        }
        pumping_ = false;
    }

private:
    enum class State : std::uint8_t { AwaitHello, Ready };
    enum class Fill : std::uint8_t { Empty, Full, Budget };

    /// Reading commands stops while our output is at high water: every command is answered, so
    /// reading more would only grow the buffer the application is not draining.
    [[nodiscard]] bool reads_paused() const noexcept {
        return state_ == State::Ready && out_.size() >= high_water_;
    }

    void on_readable() noexcept {
        for (std::size_t reads = 0; !closed_ && !closing_after_flush_; ++reads) {
            if (reads_paused()) {
                update_interest();  // Read off; flush() turns it back on when the buffer drains
                return;
            }
            if (reads >= kMaxReadsPerEvent) {
                // Budget used up: re-arm the edge so the rest is reported next iteration.
                if (!server_.loop_.rewatch(watch_, interest_)) {
                    close();
                }
                return;
            }
            if (!read_and_feed()) {
                return;
            }
        }
    }

    /// One read from the socket, handed to the decoder.
    /// @return true if more may be waiting; false if the socket is drained or the link is closed.
    [[nodiscard]] bool read_and_feed() noexcept {
        const Result<std::size_t> got = transport_.read(scratch_);
        if (!got) {
            if (got.error().code != ErrorCode::WouldBlock) {
                close();
            }
            return false;
        }
        if (*got == 0) {
            close();  // the application went away
            return false;
        }
        feed(ByteView{scratch_.data(), *got});
        return true;
    }

    void feed(ByteView input) noexcept {
        while (!closed_ && !closing_after_flush_) {
            auto next = decoder_->next(input);
            if (!next) {
                close();  // malformed or too large: a length-prefixed stream cannot resynchronise
                return;
            }
            const std::optional<ipc::Message>& message = *next;
            if (!message) {
                break;  // need more bytes
            }
            handle(*message);
        }
        if (!closed_ && out_.size() > 0) {
            static_cast<void>(flush());  // one write for all the answers to this read
        }
    }

    void handle(const ipc::Message& message) noexcept {
        if (state_ == State::AwaitHello) {
            const Handshake::Outcome outcome = server_.handshake_.on_first_message(message);
            if (!outcome.accepted) {
                closing_after_flush_ = true;  // one STATUS, then goodbye
                send(outcome.reply);
                return;
            }
            state_ = State::Ready;
            peer_minor_ = outcome.peer_minor;
            if (hello_timer_) {
                server_.loop_.cancel(*hello_timer_);
                hello_timer_.reset();
            }
            // From here on frames may be as large as the configured limit. The old decoder holds
            // no bytes (it just returned a whole message), so the rest of this read goes to the new
            // one.
            decoder_ = std::make_unique<ipc::FrameDecoder>(server_.config_.max_frame);
            send(outcome.reply);
            server_.open_intake(true);  // someone can answer now
            // Events that queued up before this application connected are waiting; the edge that
            // announced them was spent while nobody was ready to take them.
            pump();
            return;
        }
        dispatch(message);
    }

    /// A command from an application that is past HELLO.
    void dispatch(const ipc::Message& message) noexcept {
        if (const auto* data = std::get_if<ipc::SendData>(&message)) {
            send(server_.relay_.execute(*data));
        } else if (const auto* close_command = std::get_if<ipc::CloseCommand>(&message)) {
            send(server_.relay_.execute(*close_command));
        } else if (const auto* accept = std::get_if<ipc::HandshakeAccept>(&message)) {
            send(server_.relay_.execute(*accept));
        } else if (const auto* reject = std::get_if<ipc::HandshakeReject>(&message)) {
            send(server_.relay_.execute(*reject));
        } else if (std::holds_alternative<ipc::Ping>(message)) {
            send(ipc::Pong{});
        } else {
            dispatch_broker(message);
        }
    }

    /// The broker frames (IPC 0.3, ADR-0028), or a frame this side may not receive. An application
    /// that said minor < 3 cannot know the broker frames exist: a protocol error, like HELLO twice
    /// or a frame only the daemon may send.
    void dispatch_broker(const ipc::Message& message) noexcept {
        const auto* topic = std::get_if<ipc::TopicCommand>(&message);
        const auto* publish = std::get_if<ipc::Publish>(&message);
        if ((topic == nullptr && publish == nullptr) || peer_minor_ < ipc::kBrokerMinor) {
            close();
            return;
        }
        send(topic != nullptr ? server_.relay_.execute(*topic) : server_.relay_.execute(*publish));
    }

    /// Queue one message for the application. Does not write: callers flush once per batch.
    void send(const ipc::Message& message) noexcept {
        if (!ipc::encode(message, server_.config_.max_frame, out_)) {
            close();
        }
    }

    /// Encode events into `out_` until it reaches high water, the backend is empty, or the budget
    /// for one batch is spent. Nothing is taken from the backend that cannot be stored: the check
    /// comes BEFORE the poll, so no event is ever dropped here.
    [[nodiscard]] Fill fill_output() noexcept {
        for (std::size_t taken = 0; taken < kMaxEventsPerFill; ++taken) {
            if (out_.size() >= high_water_) {
                return Fill::Full;
            }
            const std::optional<HostEvent> event = server_.relay_.next_event();
            if (!event) {
                return Fill::Empty;
            }
            // The configuration guarantees the biggest message fits a frame, and out_ has room for
            // a frame below high water, so this cannot fail; if it ever does, the link is closed
            // rather than silently losing an event.
            if (!ipc::encode(to_ipc_event(*event), server_.config_.max_frame, out_)) {
                if (event->type == HostEventType::Handshake) {
                    // The question grew with what the CLIENT sent (headers, path), so it may not
                    // fit a frame. That is no reason to cut the application off: refuse the upgrade.
                    server_.refuse_oversized(*event);
                    continue;
                }
                close();
                return Fill::Empty;
            }
        }
        return Fill::Budget;
    }

    /// Write as much of `out_` as the socket takes.
    /// @return true if `out_` is now empty (and the link is still open).
    [[nodiscard]] bool flush() noexcept {
        bool progressed = false;
        while (out_.size() > 0) {
            const Result<std::size_t> sent = transport_.write(out_.view());
            if (!sent) {
                if (sent.error().code != ErrorCode::WouldBlock) {
                    close();
                    return false;
                }
                blocked_ = true;
                note_blocked(progressed);
                update_interest();
                return false;
            }
            progressed = progressed || *sent > 0;
            out_.consume(*sent);
        }
        blocked_ = false;
        cancel_stall_timer();
        update_interest();
        if (closing_after_flush_) {
            transport_.shutdown_write();
            close();
            return false;
        }
        return !closed_;
    }

    /// The socket is full and we still hold data. If the application does not read at all for
    /// stall_timeout, the link is closed; any byte it takes starts the clock again.
    void note_blocked(bool progressed) noexcept {
        if (stall_timer_ && !progressed) {
            return;  // already counting, and nothing moved
        }
        cancel_stall_timer();
        const Result<TimerHandle> timer =
            server_.loop_.run_after(server_.config_.stall_timeout, [this] {
                stall_timer_.reset();
                stalled_ = true;
                close();  // the application stopped reading
            });
        if (timer) {
            stall_timer_ = *timer;
        } else {
            close();  // cannot enforce the deadline; do not run without it
        }
    }

    void cancel_stall_timer() noexcept {
        if (stall_timer_) {
            server_.loop_.cancel(*stall_timer_);
            stall_timer_.reset();
        }
    }

    /// Read while we can accept answers; Write while the socket is full.
    void update_interest() noexcept {
        PollEvents desired = reads_paused() ? PollEvents::None : PollEvents::Read;
        if (blocked_) {
            desired = desired | PollEvents::Write;
        }
        if (desired == interest_) {
            return;
        }
        interest_ = desired;
        if (!server_.loop_.rewatch(watch_, interest_)) {
            close();
        }
    }

    void teardown() noexcept {
        if (hello_timer_) {
            server_.loop_.cancel(*hello_timer_);
            hello_timer_.reset();
        }
        cancel_stall_timer();
        if (watch_.generation != 0) {
            server_.loop_.unwatch(watch_);
            watch_ = WatchHandle{};
        }
        transport_.close();
    }

    void close() noexcept {
        if (closed_) {
            return;
        }
        closed_ = true;
        teardown();
        if (state_ == State::Ready) {
            server_.application_lost(stalled_);  // it had been told about connections
        }
        server_.release(this);
    }

    IpcServer& server_;
    PlainTransport transport_;
    std::unique_ptr<ipc::FrameDecoder> decoder_;
    const std::size_t high_water_;
    ByteBuffer out_;
    std::array<std::byte, 16384> scratch_{};
    WatchHandle watch_{};
    PollEvents interest_ = PollEvents::Read;
    std::optional<TimerHandle> hello_timer_;
    std::optional<TimerHandle> stall_timer_;
    State state_ = State::AwaitHello;
    std::uint16_t peer_minor_ = 0;  // the minor the application said in HELLO
    bool closing_after_flush_ = false;
    bool blocked_ = false;  // the socket refused a write and `out_` still holds data
    bool pumping_ = false;
    bool stalled_ = false;  // closed by the stall timer, not by the application
    bool closed_ = false;
};

// ---- the server ---------------------------------------------------------------------------------

Result<std::unique_ptr<IpcServer>>
IpcServer::create(EventLoop& loop, ISyscalls& sys, IBackend& backend, IpcServerConfig config) {
    if (config.max_frame < ipc::kFrameHeaderSize + 1) {
        return fail(ErrorCode::InvalidConfig, "ipc max_frame is too small");
    }
    Result<UnixListener> listener = UnixListener::open(
        sys, UnixListenerConfig{.path = config.socket_path, .backlog = kBacklog});
    if (!listener) {
        return std::unexpected{listener.error()};
    }
    auto server = std::make_unique<IpcServer>(
        Key{}, loop, sys, backend, std::move(config), std::move(*listener));
    server->open_intake(false);  // until an application says HELLO, nobody can answer
    const Result<WatchHandle> watch = loop.watch(server->listener_.fd(), PollEvents::Read, *server);
    if (!watch) {
        return std::unexpected{watch.error()};
    }
    server->watch_ = *watch;
    const Result<WatchHandle> events =
        loop.watch(backend.event_fd(), PollEvents::Read, server->event_source_);
    if (!events) {
        return std::unexpected{events.error()};
    }
    server->events_watch_ = *events;
    return server;
}

IpcServer::IpcServer(Key /*unused*/,
                     EventLoop& loop,
                     ISyscalls& sys,
                     IBackend& backend,
                     IpcServerConfig config,
                     UnixListener listener)
    : loop_{loop}, sys_{sys}, backend_{backend}, config_{std::move(config)},
      handshake_{config_.token, config_.max_frame, config_.handshake_hook}, relay_{backend},
      listener_{std::move(listener)} {}

IpcServer::~IpcServer() {
    epoch_.reset();  // tasks already queued for this server become no-ops
    if (retry_) {
        loop_.cancel(*retry_);
    }
    if (grace_timer_) {
        loop_.cancel(*grace_timer_);
    }
    if (close_retry_) {
        loop_.cancel(*close_retry_);
    }
    if (events_watch_) {
        loop_.unwatch(*events_watch_);
    }
    if (watch_) {
        loop_.unwatch(*watch_);
    }
    peer_.reset();
}

bool IpcServer::has_consumer() const noexcept {
    return peer_ != nullptr && !peer_->closed();
}

void IpcServer::on_event(PollEvents /*events*/) noexcept {
    accept_burst();
}

void IpcServer::on_backend_events() noexcept {
    // No application connected: leave the events where they are. They stay in the backend's queue,
    // whose own limits pause the WebSocket clients, and the next HELLO_OK starts the pump.
    if (peer_ != nullptr && !peer_->closed()) {
        peer_->pump();
    }
}

void IpcServer::open_intake(bool open) noexcept {
    if (config_.handshake_hook) {
        backend_.set_handshake_intake(open);
    }
}

void IpcServer::refuse_oversized(const HostEvent& question) noexcept {
    // 503: the server cannot ask. A failure means the server already ended the wait.
    static_cast<void>(backend_.reject_handshake(question.conn, kStatusUnavailable).has_value());
}

void IpcServer::application_lost(bool stalled) noexcept {
    open_intake(false);  // first: no new question may go to an application that is gone
    relay_.detach();
    if (relay_.stale_count() == 0) {
        return;  // it knew no connection: nothing to close
    }
    if (stalled) {
        // The application holds our output hostage; waiting would only grow the backlog.
        close_stale_connections(kCloseGoingAway, "application stalled");
        return;
    }
    // A restart is the common case: give the replacement the grace period, then close what it
    // cannot serve. Losing another application meanwhile restarts the clock for everyone.
    if (grace_timer_) {
        loop_.cancel(*grace_timer_);
        grace_timer_.reset();
    }
    const Result<TimerHandle> timer =
        loop_.run_after(config_.restart_grace, [this, weak = std::weak_ptr<char>{epoch_}] {
            if (weak.expired()) {
                return;
            }
            grace_timer_.reset();
            close_stale_connections(kCloseServiceRestart, "service restart");
        });
    if (timer) {
        grace_timer_ = *timer;
    } else {
        // No timer to wait with: close now rather than leave connections nobody will ever serve.
        close_stale_connections(kCloseServiceRestart, "service restart");
    }
}

void IpcServer::close_stale_connections(std::uint16_t code, std::string_view reason) {
    if (close_retry_) {
        loop_.cancel(*close_retry_);
        close_retry_.reset();
    }
    if (relay_.close_stale(code, reason) == 0) {
        return;
    }
    // A loop's task queue was full for some of them: they are still open. Try again shortly; each
    // round only asks the ones still refused, and the set is bounded by max_connections.
    const Result<TimerHandle> timer = loop_.run_after(
        kCloseRetry, [this, code, text = std::string{reason}, weak = std::weak_ptr<char>{epoch_}] {
            if (weak.expired()) {
                return;
            }
            close_retry_.reset();
            close_stale_connections(code, text);
        });
    if (timer) {
        close_retry_ = *timer;
    }
}

void IpcServer::rearm_event_source() noexcept {
    if (events_watch_ && !loop_.rewatch(*events_watch_, PollEvents::Read)) {
        // Cannot ask the loop for another report; do not stall silently. Try again next turn.
        const Result<void> queued = loop_.defer([this, weak = std::weak_ptr<char>{epoch_}] {
            if (!weak.expired()) {
                on_backend_events();
            }
        });
        static_cast<void>(queued);
    }
}

void IpcServer::accept_burst() noexcept {
    for (std::size_t accepted = 0; accepted < kMaxAcceptsPerEvent; ++accepted) {
        const SysResult result =
            sys_.accept4(listener_.fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (result.err == EINTR || result.err == ECONNABORTED) {
            continue;
        }
        if (result.err == EAGAIN) {
            return;
        }
        if (result.err != 0) {
            schedule_retry();  // out of descriptors or similar: wait, do not spin
            return;
        }
        admit(Fd{static_cast<int>(result.value)});
    }
    // Budget used up: re-arm the edge so the remaining connects are reported next iteration.
    if (watch_ && !loop_.rewatch(*watch_, PollEvents::Read)) {
        schedule_retry();
    }
}

void IpcServer::admit(Fd client) noexcept {
    // 1. Who is on the other end, according to the kernel, before a single byte is read.
    std::uint32_t uid = 0;
    if (sys_.peer_uid(client.get(), uid).err != 0 || uid != sys_.effective_uid()) {
        return;  // closing the descriptor is the whole answer to a stranger
    }
    // 2. One application at a time. First let the current one show that it is gone, if it is.
    if (peer_ != nullptr && !peer_->closed()) {
        peer_->look_now();
    }
    if (peer_ != nullptr && peer_->closed()) {
        peer_.reset();
    }
    if (peer_ != nullptr) {
        reject_busy(std::move(client));
        return;
    }
    auto peer = std::make_unique<Peer>(*this, std::move(client));
    if (!peer->start()) {
        return;  // the peer's destructor closes the socket
    }
    peer_ = std::move(peer);
}

void IpcServer::reject_busy(Fd client) noexcept {
    // Best effort: a few bytes into an empty socket buffer. If it does not go through, the close
    // that follows still tells the application something is wrong.
    ByteBuffer out{ipc::kFrameHeaderSize + 16};
    const ipc::Status status{.request = 0,
                             .status = static_cast<std::uint8_t>(WSRV_ERR_BACKPRESSURE)};
    if (ipc::encode(status, config_.max_frame, out)) {
        PlainTransport transport{std::move(client), sys_};
        const Result<std::size_t> sent = transport.write(out.view());
        static_cast<void>(sent);  // best effort: the close that follows says it either way
        transport.shutdown_write();
    }
}

void IpcServer::schedule_retry() noexcept {
    if (retry_ || !watch_) {
        return;
    }
    const Result<TimerHandle> timer =
        loop_.run_after(kAcceptRetry, [this, weak = std::weak_ptr<char>{epoch_}] {
            if (weak.expired()) {
                return;
            }
            retry_.reset();
            if (watch_ && !loop_.rewatch(*watch_, PollEvents::Read)) {
                schedule_retry();  // still cannot re-arm: try again later
            }
        });
    if (timer) {
        retry_ = *timer;
    }
}

void IpcServer::release(const Peer* peer) noexcept {
    // The peer asked for this from inside its own callback, so destroy it at the end of the loop
    // iteration. If the loop cannot take the task, admit() resets a closed peer when the next
    // connection arrives, so the slot is not lost either way.
    const Result<void> queued = loop_.defer([this, weak = std::weak_ptr<char>{epoch_}, peer] {
        if (weak.expired()) {
            return;
        }
        if (peer_.get() == peer && peer_->closed()) {
            peer_.reset();
        }
    });
    static_cast<void>(queued);  // see above: admit() covers a refused task
}

}  // namespace wsrv::wsrvd
