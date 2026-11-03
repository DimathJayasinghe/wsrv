// SPDX-License-Identifier: Apache-2.0
#include "wsrvd/ipc_server.hpp"

#include "base/byte_buffer.hpp"
#include "net/fd.hpp"
#include "net/plain_transport.hpp"
#include "wsrv/wsrv.h"

#include <array>
#include <cerrno>
#include <cstddef>
#include <utility>
#include <variant>

#include <sys/socket.h>

#include "ipc/frame_decoder.hpp"
#include "ipc/message.hpp"

namespace wsrv::wsrvd {

namespace {

constexpr int kBacklog = 4;  // one consumer: a handful of waiting connects is plenty
constexpr std::size_t kMaxAcceptsPerEvent = 16;
constexpr std::size_t kMaxReadsPerEvent = 16;  // fairness budget (EC-N07, EC-N08)
constexpr std::chrono::milliseconds kAcceptRetry{100};

}  // namespace

// ---- one connected application ------------------------------------------------------------------

/// The state of one IPC connection: bytes in, decoder, handshake, bytes out. Everything runs on the
/// loop thread. A Peer never destroys itself from inside its own callback: close() only marks it
/// closed and asks the server to release it at the end of the loop iteration (ADR-0013).
class IpcServer::Peer final : public IEventHandler {
public:
    Peer(IpcServer& server, Fd fd)
        : server_{server}, transport_{std::move(fd), server.sys_},
          decoder_{std::make_unique<ipc::FrameDecoder>(kHelloMaxFrame)},
          out_{2 * static_cast<std::size_t>(server.config_.max_frame)} {}
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
            flush();
        }
        if (!closed_ && (has(events, PollEvents::Read) || has(events, PollEvents::Hangup) ||
                         has(events, PollEvents::Error))) {
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

private:
    enum class State : std::uint8_t { AwaitHello, Ready };

    void on_readable() noexcept {
        for (std::size_t reads = 0; !closed_ && !closing_after_flush_; ++reads) {
            if (reads >= kMaxReadsPerEvent) {
                // Budget used up: re-arm the edge so the rest is reported next iteration.
                if (!server_.loop_.rewatch(watch_, interest_)) {
                    close();
                }
                return;
            }
            const Result<std::size_t> got = transport_.read(scratch_);
            if (!got) {
                if (got.error().code != ErrorCode::WouldBlock) {
                    close();
                }
                return;
            }
            if (*got == 0) {
                close();  // the application went away
                return;
            }
            feed(ByteView{scratch_.data(), *got});
        }
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
                return;  // need more bytes
            }
            handle(*message);
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
            if (hello_timer_) {
                server_.loop_.cancel(*hello_timer_);
                hello_timer_.reset();
            }
            // From here on frames may be as large as the configured limit. The old decoder holds
            // no bytes (it just returned a whole message), so the rest of this read goes to the new
            // one.
            decoder_ = std::make_unique<ipc::FrameDecoder>(server_.config_.max_frame);
            send(outcome.reply);
            return;
        }
        if (std::holds_alternative<ipc::Ping>(message)) {
            send(ipc::Pong{});
            return;
        }
        close();  // HELLO twice, or a command (slice 3): not understood yet
    }

    void send(const ipc::Message& message) noexcept {
        if (!ipc::encode(message, server_.config_.max_frame, out_)) {
            close();
            return;
        }
        flush();
    }

    void flush() noexcept {
        while (out_.size() > 0) {
            const Result<std::size_t> sent = transport_.write(out_.view());
            if (!sent) {
                if (sent.error().code == ErrorCode::WouldBlock) {
                    want_write(true);
                } else {
                    close();
                }
                return;
            }
            out_.consume(*sent);
        }
        want_write(false);
        if (closing_after_flush_) {
            transport_.shutdown_write();
            close();
        }
    }

    void want_write(bool wanted) noexcept {
        const PollEvents desired =
            wanted ? (PollEvents::Read | PollEvents::Write) : PollEvents::Read;
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
        server_.release(this);
    }

    IpcServer& server_;
    PlainTransport transport_;
    std::unique_ptr<ipc::FrameDecoder> decoder_;
    ByteBuffer out_;
    std::array<std::byte, 16384> scratch_{};
    WatchHandle watch_{};
    PollEvents interest_ = PollEvents::Read;
    std::optional<TimerHandle> hello_timer_;
    State state_ = State::AwaitHello;
    bool closing_after_flush_ = false;
    bool closed_ = false;
};

// ---- the server ---------------------------------------------------------------------------------

Result<std::unique_ptr<IpcServer>>
IpcServer::create(EventLoop& loop, ISyscalls& sys, IpcServerConfig config) {
    if (config.max_frame < ipc::kFrameHeaderSize + 1) {
        return fail(ErrorCode::InvalidConfig, "ipc max_frame is too small");
    }
    Result<UnixListener> listener = UnixListener::open(
        sys, UnixListenerConfig{.path = config.socket_path, .backlog = kBacklog});
    if (!listener) {
        return std::unexpected{listener.error()};
    }
    auto server =
        std::make_unique<IpcServer>(Key{}, loop, sys, std::move(config), std::move(*listener));
    const Result<WatchHandle> watch = loop.watch(server->listener_.fd(), PollEvents::Read, *server);
    if (!watch) {
        return std::unexpected{watch.error()};
    }
    server->watch_ = *watch;
    return server;
}

IpcServer::IpcServer(
    Key /*unused*/, EventLoop& loop, ISyscalls& sys, IpcServerConfig config, UnixListener listener)
    : loop_{loop}, sys_{sys}, config_{std::move(config)},
      handshake_{config_.token, config_.max_frame}, listener_{std::move(listener)} {}

IpcServer::~IpcServer() {
    epoch_.reset();  // tasks already queued for this server become no-ops
    if (retry_) {
        loop_.cancel(*retry_);
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
