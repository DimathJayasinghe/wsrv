// SPDX-License-Identifier: Apache-2.0
#include "net/acceptor.hpp"

#include <cerrno>
#include <exception>

#include <netinet/in.h>
#include <netinet/tcp.h>

namespace wsrv {

Acceptor::Acceptor(EventLoop& loop,
                   TcpListener& listener,
                   ISyscalls& sys,
                   AcceptCallback on_accept,
                   AcceptorConfig config)
    : loop_{loop}, listener_{listener}, sys_{sys}, on_accept_{std::move(on_accept)},
      config_{config} {}

Acceptor::~Acceptor() {
    stop();
}

Result<void> Acceptor::start() {
    const Result<long> spare = retrying([&] { return sys_.open_null(); }, "open /dev/null");
    if (!spare) {
        return std::unexpected{spare.error()};
    }
    reserve_.reset(static_cast<int>(*spare));
    const Result<WatchHandle> watch = loop_.watch(listener_.fd(), PollEvents::Read, *this);
    if (!watch) {
        return std::unexpected{watch.error()};
    }
    watch_ = *watch;
    return {};
}

void Acceptor::stop() {
    epoch_ = std::make_shared<char>();  // any task still queued for the old epoch becomes a no-op
    if (backoff_) {
        static_cast<void>(loop_.cancel(*backoff_));
        backoff_.reset();
    }
    if (watch_) {
        loop_.unwatch(*watch_);
        watch_.reset();
    }
}

void Acceptor::on_event(PollEvents /*events*/) noexcept {
    accept_burst();
}

SysResult Acceptor::accept_one(sockaddr_storage& peer) {
    SysResult result;
    for (std::size_t attempt = 0; attempt < kMaxEintrRetries; ++attempt) {
        socklen_t length = sizeof(peer);
        result =
            sys_.accept4(listener_.fd(), as_sockaddr(peer), &length, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (result.err != EINTR) {
            break;
        }
    }
    return result;
}

void Acceptor::deliver(Fd client, const sockaddr_storage& peer) noexcept {
    const int on = 1;  // best effort: a failure only costs latency
    const bool nodelay =
        sys_.setsockopt(client.get(), {IPPROTO_TCP, TCP_NODELAY}, &on, sizeof(on)).err == 0;
    static_cast<void>(nodelay);
    const Result<Endpoint> from = from_sockaddr(peer);
    try {
        on_accept_(std::move(client), from.value_or(Endpoint{}));
    } catch (const std::exception&) {
        note_error();  // the callback broke its contract; the connection is lost, the loop is not
    }
}

void Acceptor::accept_burst() noexcept {
    if (!watch_) {
        return;  // paused or stopped
    }
    for (std::size_t accepted = 0; accepted < config_.max_accepts_per_event;) {
        sockaddr_storage peer{};
        const SysResult result = accept_one(peer);
        if (result.err == 0) {
            deliver(Fd{static_cast<int>(result.value)}, peer);
            ++accepted;
            continue;
        }
        switch (result.err) {
        case EAGAIN:
            return;  // queue drained: wait for the next edge
        case ECONNABORTED:
        case EPROTO:
            continue;  // that client is gone; try the next one (EC-N06)
        case EMFILE:
        case ENFILE:
            if (config_.metrics != nullptr) {
                config_.metrics->add(Metric::AcceptErrorsEmfile);
            }
            shed_pending();
            pause();
            return;
        default:
            note_error();
            pause();
            return;
        }
    }
    // Budget used up: connections may still be queued but the edge was consumed. Re-arm so the
    // kernel reports them again in the next iteration.
    if (watch_ && !loop_.rewatch(*watch_, PollEvents::Read)) {
        note_error();
        pause();  // cannot re-arm the listener: back off instead of leaving queued clients deaf
    }
}

void Acceptor::note_error() noexcept {
    ++error_count_;
    if (config_.metrics != nullptr) {
        config_.metrics->add(Metric::AcceptErrorsOther);
    }
}

/// The reserve-descriptor trick (EC-N05): free one descriptor, use it to take the connections that
/// are waiting, close them at once, then take the spare back.
void Acceptor::shed_pending() noexcept {
    reserve_.reset();
    for (std::size_t shed = 0; shed < config_.max_accepts_per_event; ++shed) {
        sockaddr_storage peer{};
        const SysResult result = accept_one(peer);
        if (result.err != 0) {
            break;  // nothing waiting, or still no descriptor
        }
        const Fd doomed{static_cast<int>(result.value)};  // closed at the end of this iteration
        ++shed_count_;
    }
    if (const SysResult spare = sys_.open_null(); spare.err == 0) {
        reserve_.reset(static_cast<int>(spare.value));
    }
}

void Acceptor::pause() noexcept {
    if (watch_) {
        loop_.unwatch(*watch_);
        watch_.reset();
    }
    schedule_backoff();
}

/// Arm the back-off timer. If that is impossible (timer table full, out of memory) the attempt is
/// repeated from a posted task, i.e. paced by the loop, never by re-entering accept on this stack.
void Acceptor::schedule_backoff() noexcept {
    try {
        const Result<TimerHandle> timer =
            loop_.run_after(config_.emfile_backoff, [this] { resume(); });
        if (timer) {
            backoff_ = *timer;
            return;
        }
        note_error();
        const std::weak_ptr<char> token = epoch_;
        const Result<void> queued = loop_.post([this, token] {
            if (token.lock()) {
                schedule_backoff();
            }
        });
        if (!queued) {
            note_error();  // nothing left to try: stay paused until stop() / start()
        }
    } catch (const std::exception&) {
        note_error();  // allocating the callback failed: same outcome, stay paused
    }
}

void Acceptor::resume() noexcept {
    backoff_.reset();
    const Result<WatchHandle> watch = loop_.watch(listener_.fd(), PollEvents::Read, *this);
    if (!watch) {
        note_error();
        pause();
        return;
    }
    watch_ = *watch;
    accept_burst();  // the edge may have been missed while we were not watching
}

}  // namespace wsrv
