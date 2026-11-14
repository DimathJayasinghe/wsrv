// SPDX-License-Identifier: Apache-2.0
#include "server/admin_endpoint.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include <sys/socket.h>

namespace wsrv {

/// One admin request: read the head, answer once, close. Owned by the endpoint; it removes
/// itself through retire() (deferred, so it is never destroyed inside its own callback).
class AdminEndpoint::Client final : public IEventHandler {
public:
    Client(AdminEndpoint& owner, Fd fd) : owner_{owner}, fd_{std::move(fd)} {}

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
        // Edge-triggered: read until the kernel has nothing more or the head is complete.
        for (;;) {
            const std::size_t room = head_.size() - used_;
            if (room == 0) {
                answer_and_finish(std::string_view{head_.data(), used_}, /*complete=*/false);
                return;
            }
            const SysResult got = owner_.sys_.read(fd_.get(), head_.data() + used_, room);
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
            used_ += static_cast<std::size_t>(got.value);
            const std::string_view seen{head_.data(), used_};
            if (admin_head_complete(seen)) {
                answer_and_finish(seen, /*complete=*/true);
                return;
            }
        }
    }

private:
    void on_deadline() noexcept {
        timer_.reset();  // it fired
        finish_and_retire();
    }

    /// One non-blocking send. A kernel that does not take it all gets a closed connection: a
    /// truncated page is never left looking complete (its Content-Length would not match).
    void answer_and_finish(std::string_view head, bool complete) noexcept {
        try {
            const std::string response =
                complete ? admin_response(head, owner_.sources_) : admin_bad_request();
            static_cast<void>(owner_.sys_.send(
                fd_.get(), response.data(), response.size(), MSG_NOSIGNAL | MSG_DONTWAIT));
        } catch (const std::bad_alloc&) {
            finish_and_retire();  // out of memory: no answer, the request still ends
            return;
        }
        finish_and_retire();
    }

    void finish_and_retire() noexcept {
        finish();
        owner_.retire(*this);
    }

    AdminEndpoint& owner_;
    Fd fd_;
    std::optional<WatchHandle> watch_;
    std::optional<TimerHandle> timer_;
    std::array<char, kMaxAdminHeadBytes> head_{};
    std::size_t used_ = 0;
    bool done_ = false;
};

Result<std::unique_ptr<AdminEndpoint>> AdminEndpoint::create(EventLoop& loop,
                                                             ISyscalls& sys,
                                                             const AdminConfig& config,
                                                             AdminSources sources) {
    std::unique_ptr<AdminEndpoint> endpoint;
    try {
        endpoint = std::make_unique<AdminEndpoint>(Key{}, loop, sys, config, std::move(sources));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "admin: out of memory");
    }
    if (const Result<void> opened = endpoint->open(); !opened) {
        return std::unexpected{opened.error()};
    }
    return endpoint;
}

/// Bind, listen and start accepting (bind errors surface here, EC-N20).
Result<void> AdminEndpoint::open() {
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
        return fail(ErrorCode::ResourceExhausted, "admin: out of memory");
    }
    return acceptor_->start();
}

AdminEndpoint::AdminEndpoint(Key /*unused*/,
                             EventLoop& loop,
                             ISyscalls& sys,
                             const AdminConfig& config,
                             AdminSources sources)
    : loop_{loop}, sys_{sys}, config_{config}, sources_{std::move(sources)} {}

AdminEndpoint::~AdminEndpoint() {
    if (acceptor_) {
        acceptor_->stop();
    }
    for (const std::unique_ptr<Client>& client : clients_) {
        client->finish();  // unwatch, cancel the deadline, close
    }
}

void AdminEndpoint::on_accept(Fd client) {
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
void AdminEndpoint::sweep() {
    std::erase_if(clients_, [](const std::unique_ptr<Client>& each) { return each->done(); });
}

/// A client finished inside its own callback: destroy it at the end of the loop iteration. If the
/// loop cannot take the task, the finished client stays in the list (it is inert) and is swept by
/// the next accept; the list is bounded by 2 * max_connections.
void AdminEndpoint::retire(Client& /*client*/) {
    static_cast<void>(loop_.defer([this] { sweep(); }).has_value());
}

}  // namespace wsrv
