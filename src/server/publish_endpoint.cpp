// SPDX-License-Identifier: Apache-2.0
#include "server/publish_endpoint.hpp"

#include "base/bytes.hpp"
#include "base/constant_time.hpp"
#include "http/publish_request.hpp"
#include "http/request_parser.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <sys/socket.h>

namespace wsrv {

/// One publish request: read the head, check who is asking, read the body, publish once, answer
/// once, close. Owned by the endpoint; it removes itself through retire() (deferred, so it is
/// never destroyed inside its own callback).
class PublishEndpoint::Client final : public IEventHandler {
public:
    Client(PublishEndpoint& owner, Fd fd)
        : owner_{owner}, fd_{std::move(fd)},
          parser_{HttpLimits{.max_request_line = kMaxPublishHeadBytes,
                             .max_total_bytes = kMaxPublishHeadBytes,
                             .max_headers = 16}} {}

    /// Register with the loop and start the deadline. On failure the caller drops the client.
    [[nodiscard]] bool start() {
        const Result<WatchHandle> watch = owner_.loop_.watch(fd_.get(), PollEvents::Read, *this);
        if (!watch) {
            return false;
        }
        watch_ = *watch;
        const Result<TimerHandle> timer =
            owner_.loop_.run_after(owner_.config_.request_timeout, [this] { on_deadline(); });
        if (!timer) {
            owner_.loop_.unwatch(*watch_);
            watch_.reset();
            return false;
        }
        timer_ = *timer;
        return true;
    }

    [[nodiscard]] bool done() const noexcept {
        return done_;
    }

    /// Stop everything and close; idempotent. Safe from the destructor of the endpoint.
    void finish() noexcept {
        if (done_) {
            return;
        }
        done_ = true;
        if (timer_) {
            static_cast<void>(owner_.loop_.cancel(*timer_));
            timer_.reset();
        }
        if (watch_) {
            owner_.loop_.unwatch(*watch_);
            watch_.reset();
        }
        fd_.reset();
    }

    void on_event(PollEvents /*events*/) noexcept override {
        if (done_) {
            return;
        }
        try {
            pump();
        } catch (const std::bad_alloc&) {
            finish_and_retire();  // out of memory: no answer, the request still ends
        }
    }

private:
    enum class Phase : std::uint8_t { Head, Body };

    void on_deadline() noexcept {
        timer_.reset();  // it fired
        finish_and_retire();
    }

    /// Edge-triggered: read until the kernel has nothing more, the request is answered, or the
    /// peer is gone.
    void pump() {
        while (!done_) {
            const bool head = phase_ == Phase::Head;
            char* into = head ? head_.data() + used_ : body_.data() + body_used_;
            const std::size_t room = head ? head_.size() - used_ : body_.size() - body_used_;
            if (room == 0) {
                reply(400, "request head too large\n");
                return;
            }
            const SysResult got = owner_.sys_.read(fd_.get(), into, room);
            if (got.err == EINTR) {
                continue;
            }
            if (got.err == EAGAIN || got.err == EWOULDBLOCK) {
                return;  // wait for more bytes (or the deadline)
            }
            if (got.err != 0 || got.value == 0) {
                finish_and_retire();  // reset or end of stream before a full request
                return;
            }
            const auto count = static_cast<std::size_t>(got.value);
            if (head) {
                on_head_bytes(count);
            } else {
                on_body_bytes(count);
            }
        }
    }

    void on_head_bytes(std::size_t count) {
        const ByteView fresh = as_bytes(std::string_view{head_.data() + used_, count});
        used_ += count;
        const HttpResult<ParseProgress> progress = parser_.feed(fresh);
        if (!progress) {
            reply(400, "bad request\n");  // 414, 431, 505 and friends are one answer here
            return;
        }
        if (progress->complete) {
            const std::size_t surplus = count - progress->consumed;
            begin_body(
                ByteView{as_bytes(std::string_view{head_.data() + used_ - surplus, surplus})});
        }
    }

    /// The head is complete. Check in this order, and stop at the first failure: what the request
    /// says, who is asking, how much it wants us to hold, whether we are still serving, and only
    /// then reserve the body.
    void begin_body(ByteView surplus) {
        const HttpResult<PublishRequest> request = interpret_publish(parser_.request());
        if (!request) {
            reply(request.error().status, std::string{request.error().reason} + "\n");
            return;
        }
        if (!constant_time_equal(as_bytes(std::string_view{request->bearer}),
                                 as_bytes(std::string_view{owner_.sources_.token}))) {
            reply(401, "credential required\n");  // the same words as a missing one
            return;
        }
        if (request->content_length > owner_.config_.max_body_bytes) {
            reply(413, "body too large\n");
            return;
        }
        if (!owner_.accepting_()) {
            reply(503, "shutting down\n");
            return;
        }
        if (surplus.size() > request->content_length) {
            reply(400, "bytes after the body\n");  // pipelining is not supported
            return;
        }
        request_ = *request;
        body_.resize(request->content_length);  // bounded by max_body_bytes, after authentication
        if (!surplus.empty()) {
            std::memcpy(body_.data(), surplus.data(), surplus.size());
        }
        body_used_ = surplus.size();
        phase_ = Phase::Body;
        if (body_used_ == body_.size()) {
            publish_now();
        }
    }

    void on_body_bytes(std::size_t count) {
        body_used_ += count;
        if (body_used_ == body_.size()) {
            publish_now();
        }
    }

    void publish_now() {
        const PublishMessage message{
            request_->topic, as_bytes(std::string_view{body_}), request_->binary, request_->except};
        const PublishAnswer answer = publish_answer(owner_.sources_.publish(message));
        reply(answer.status, answer.body);
    }

    /// Count it, send it in one non-blocking call, close. A kernel that does not take it all gets a
    /// closed connection: a truncated answer is never left looking complete.
    void reply(int status, std::string_view body) {
        if (const std::optional<Metric> metric = publish_metric(status)) {
            owner_.metrics_.add(*metric);
        }
        const std::string response = publish_response(status, body);
        static_cast<void>(owner_.sys_.send(
            fd_.get(), response.data(), response.size(), MSG_NOSIGNAL | MSG_DONTWAIT));
        finish_and_retire();
    }

    void finish_and_retire() noexcept {
        finish();
        owner_.retire(*this);
    }

    PublishEndpoint& owner_;
    Fd fd_;
    std::optional<WatchHandle> watch_;
    std::optional<TimerHandle> timer_;
    HttpRequestParser parser_;
    std::array<char, kMaxPublishHeadBytes> head_{};
    std::size_t used_ = 0;
    std::string body_;  // sized only after the credential and the length were accepted
    std::size_t body_used_ = 0;
    std::optional<PublishRequest> request_;
    Phase phase_ = Phase::Head;
    bool done_ = false;
};

Result<std::unique_ptr<PublishEndpoint>> PublishEndpoint::create(EventLoop& loop,
                                                                 ISyscalls& sys,
                                                                 const PublishConfig& config,
                                                                 const PublishSources& sources,
                                                                 IMetrics& metrics,
                                                                 std::function<bool()> accepting) {
    if (sources.token.size() < kMinPublishTokenBytes || !sources.publish || !accepting) {
        return fail(ErrorCode::InvalidConfig, "publish: needs a token of at least 32 bytes");
    }
    std::unique_ptr<PublishEndpoint> endpoint;
    try {
        endpoint = std::make_unique<PublishEndpoint>(
            Key{}, loop, sys, config, sources, metrics, std::move(accepting));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "publish: out of memory");
    }
    if (const Result<void> opened = endpoint->open(); !opened) {
        return std::unexpected{opened.error()};
    }
    return endpoint;
}

/// Bind, listen and start accepting (bind errors surface here, EC-N20).
Result<void> PublishEndpoint::open() {
    auto listener = TcpListener::open(sys_, ListenerConfig{config_.listen});
    if (!listener) {
        return std::unexpected{listener.error()};
    }
    try {
        listener_ = std::make_unique<TcpListener>(std::move(*listener));
        clients_.reserve(config_.max_connections);
        acceptor_ = std::make_unique<Acceptor>(
            loop_,
            *listener_,
            sys_,
            [this](Fd client, Endpoint /*peer*/) { on_accept(std::move(client)); },
            AcceptorConfig{});
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "publish: out of memory");
    }
    return acceptor_->start();
}

PublishEndpoint::PublishEndpoint(Key /*unused*/,
                                 EventLoop& loop,
                                 ISyscalls& sys,
                                 const PublishConfig& config,
                                 const PublishSources& sources,
                                 IMetrics& metrics,
                                 std::function<bool()> accepting)
    : loop_{loop}, sys_{sys}, config_{config}, sources_{sources}, metrics_{metrics},
      accepting_{std::move(accepting)} {}

PublishEndpoint::~PublishEndpoint() {
    if (acceptor_) {
        acceptor_->stop();
    }
    for (const std::unique_ptr<Client>& client : clients_) {
        client->finish();  // unwatch, cancel the deadline, close
    }
}

void PublishEndpoint::on_accept(Fd client) {
    sweep();
    const auto live = static_cast<std::size_t>(std::ranges::count_if(
        clients_, [](const std::unique_ptr<Client>& each) { return !each->done(); }));
    if (live >= config_.max_connections || clients_.size() >= 2 * config_.max_connections) {
        return;  // over the bound: the descriptor is closed with `client`
    }
    try {
        clients_.push_back(std::make_unique<Client>(*this, std::move(client)));
    } catch (const std::bad_alloc&) {
        return;
    }
    if (!clients_.back()->start()) {
        clients_.back()->finish();
        sweep();
    }
}

/// Forget the clients that are done. Called when a client arrives and from the deferred retire.
void PublishEndpoint::sweep() {
    std::erase_if(clients_, [](const std::unique_ptr<Client>& each) { return each->done(); });
}

/// A client finished inside its own callback: destroy it at the end of the loop iteration. If the
/// loop cannot take the task, the finished client stays in the list (it is inert) and is swept by
/// the next accept; the list is bounded by 2 * max_connections.
void PublishEndpoint::retire(Client& /*client*/) {
    static_cast<void>(loop_.defer([this] { sweep(); }).has_value());
}

}  // namespace wsrv
