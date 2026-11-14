// SPDX-License-Identifier: Apache-2.0
#include "server/loop_shard.hpp"

#include "net/plain_transport.hpp"

#include <new>
#include <utility>

namespace wsrv {

namespace {

/// One read buffer shared by all connections of the loop (see ConnectionContext::scratch).
constexpr std::size_t kScratchSize = std::size_t{16} * 1024;

/// Slack on top of max_connections for the loop's own registrations (listener, signals).
constexpr std::size_t kLoopSlack = 16;

/// A connection holds at most a handshake or tick timer plus a write-stall timer.
constexpr std::size_t kTimersPerConnection = 3;

/// Every shard is sized for the whole cap, not cap / threads: the distribution is not guaranteed
/// to be even (ADR-0014 section 3), and an unused slot costs little.
[[nodiscard]] EventLoopConfig loop_config_for(std::size_t max_connections) {
    EventLoopConfig loop;
    loop.max_watches = max_connections + kLoopSlack;
    loop.max_deferred = max_connections + kLoopSlack;  // one retire per connection
    loop.max_timers = kTimersPerConnection * max_connections + kLoopSlack;
    return loop;
}

}  // namespace

Result<std::unique_ptr<LoopShard>> LoopShard::create(const ShardShared& shared,
                                                     std::function<void()> on_drained) {
    auto loop =
        EventLoop::create(shared.sys, shared.clock, loop_config_for(shared.config.max_connections));
    if (!loop) {
        return std::unexpected{loop.error()};
    }
    try {
        return std::make_unique<LoopShard>(Key{}, shared, std::move(*loop), std::move(on_drained));
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory creating a loop");
    }
}

LoopShard::LoopShard(Key /*unused*/,
                     const ShardShared& shared,
                     std::unique_ptr<EventLoop> loop,
                     std::function<void()> on_drained)
    : shared_{shared}, on_drained_{std::move(on_drained)}, loop_{std::move(loop)} {
    table_ = std::make_unique<ConnectionTable>(*loop_, [this] { on_removed(); });
    host_ = std::make_unique<LoopHost>(*loop_, shared_.clock, shared_.logger, *table_);
    scratch_.resize(kScratchSize);
    link_ = std::make_shared<LoopLink>(*loop_, *table_);
    context_ = std::make_unique<ConnectionContext>(ConnectionContext{*host_,
                                                                     shared_.config,
                                                                     shared_.negotiator,
                                                                     shared_.factory,
                                                                     MutableByteView{scratch_},
                                                                     link_,
                                                                     shared_.decider,
                                                                     shared_.pending});
}

LoopShard::~LoopShard() {
    // A connection that is still open here (forced shutdown, stop(), a loop that failed) owes its
    // handler one on_close: "every Open is followed by exactly one Close" is part of the host
    // contract (wsrv.h). Only this thread may run the handler, so each shard reports its own
    // connections, whatever way the server ended. The loop is no longer running: the retirements
    // this queues are never executed, the table below frees the connections. The link is closed
    // first (see the class comment): a foreign thread's handle gets Closing from now on.
    link_->close();
    abort_all();
}

void LoopShard::adopt(Fd client, const Endpoint& peer, ConnectionId id) {
    Connection* connection = nullptr;
    try {
        std::unique_ptr<ITransport> transport =
            std::make_unique<PlainTransport>(std::move(client), shared_.sys);
        if (shared_.wrapper != nullptr) {
            transport = shared_.wrapper->wrap(std::move(transport));
            if (!transport) {
                shared_.live.fetch_sub(1);  // the socket was closed with the failed wrapper
                shared_.logger.log(LogLevel::Error, "server: cannot wrap a connection's transport");
                return;
            }
        }
        connection = &table_->insert(
            std::make_unique<Connection>(id, peer, std::move(transport), *context_));
    } catch (const std::exception&) {
        shared_.live.fetch_sub(1);  // never reached the table: nothing will give the slot back
        shared_.logger.log(LogLevel::Error, "server: out of memory accepting a connection");
        return;
    }
    static_cast<void>(connection->start().has_value());  // on failure it is already retired
}

void LoopShard::begin_shutdown() {
    shutting_down_ = true;
    table_->for_each([](Connection& connection) { connection.begin_shutdown(); });
    if (table_->empty()) {
        report_drained();
    }
}

void LoopShard::abort_all() {
    table_->for_each([](Connection& connection) { connection.abort(); });
}

void LoopShard::on_removed() {
    shared_.live.fetch_sub(1);
    if (shutting_down_ && table_->empty()) {
        report_drained();
    }
}

void LoopShard::report_drained() {
    if (!drained_reported_ && on_drained_) {
        drained_reported_ = true;
        on_drained_();
    }
}

}  // namespace wsrv
