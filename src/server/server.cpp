// SPDX-License-Identifier: Apache-2.0
#include "server/server.hpp"

#include "base/assert.hpp"

#include <array>
#include <csignal>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <utility>

namespace wsrv {

Server::Server(Key /*unused*/, ServerConfig config, const ServerDeps& deps)
    : config_{std::move(config)}, factory_{*deps.factory}, decider_{deps.decider},
      sys_{deps.sys != nullptr ? *deps.sys : static_cast<ISyscalls&>(own_sys_)},
      clock_{deps.clock != nullptr ? *deps.clock : static_cast<const IClock&>(own_clock_)},
      logger_{deps.logger != nullptr ? *deps.logger : static_cast<ILogger&>(own_logger_)} {}

Server::~Server() = default;

Result<std::unique_ptr<Server>> Server::create(ServerConfig config, const ServerDeps& deps) {
    if (deps.factory == nullptr) {
        return fail(ErrorCode::InvalidArgument, "server: a handler factory is required");
    }
    if (const Result<void> valid = validate(config); !valid) {
        return std::unexpected{valid.error()};
    }
    std::unique_ptr<Server> server;
    try {
        server = std::make_unique<Server>(Key{}, std::move(config), deps);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    if (const Result<void> ready = server->init(); !ready) {
        return std::unexpected{ready.error()};
    }
    return server;
}

/// The fallible half of construction, in dependency order: negotiator, loop 0's shard, listener
/// (bind errors surface here, EC-N20), acceptor. The other loops start in run().
Result<void> Server::init() {
    try {
        negotiator_ = std::make_unique<HandshakeNegotiator>(config_.handshake);
        pending_ = std::make_unique<PendingGate>(effective_max_pending_handshakes(config_));
        shared_ = std::make_unique<ShardShared>(ShardShared{config_,
                                                            *negotiator_,
                                                            factory_,
                                                            sys_,
                                                            clock_,
                                                            logger_,
                                                            live_,
                                                            decider_,
                                                            pending_.get()});
    } catch (const std::invalid_argument&) {
        return fail(ErrorCode::InvalidConfig, "server: invalid handshake configuration");
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    auto shard = LoopShard::create(*shared_, [this] { on_shard_drained(); });
    if (!shard) {
        return std::unexpected{shard.error()};
    }
    main_ = std::move(*shard);

    auto listener = TcpListener::open(sys_, ListenerConfig{config_.listen});
    if (!listener) {
        return std::unexpected{listener.error()};
    }
    listener_ = std::make_unique<TcpListener>(std::move(*listener));

    acceptor_ = std::make_unique<Acceptor>(
        main_->loop(),
        *listener_,
        sys_,
        [this](Fd client, Endpoint peer) { on_accept(std::move(client), peer); },
        AcceptorConfig{});
    return acceptor_->start();
}

// ---- running ------------------------------------------------------------------------------------

Result<void> Server::run() {
    if (const Result<void> started = start_workers(); !started) {
        stop_workers();
        return started;
    }
    const Result<void> result = main_->loop().run();
    stop_workers();  // loop 0 decides when the others end: they are stopped here, then joined
    return result;
}

Result<void> Server::start_workers() {
    for (std::size_t i = 1; i < config_.threads; ++i) {
        auto worker = WorkerThread::start(*shared_, [this] { on_shard_drained(); });
        if (!worker) {
            return std::unexpected{worker.error()};
        }
        try {
            workers_.push_back(std::move(*worker));
        } catch (const std::bad_alloc&) {
            return fail(ErrorCode::ResourceExhausted, "server: out of memory starting loops");
        }
    }
    return {};
}

void Server::stop_workers() {
    for (const std::unique_ptr<WorkerThread>& worker : workers_) {
        worker->stop_loop();
    }
    workers_.clear();  // releases and joins each thread; its shard dies on its own thread
}

Result<void> Server::install_signal_handlers() {
    const std::array<int, 2> signals{SIGINT, SIGTERM};
    auto watcher = SignalWatcher::create(
        main_->loop(), sys_, signals, [this](int signo) { on_signal(signo); });
    if (!watcher) {
        return std::unexpected{watcher.error()};
    }
    signals_ = std::move(*watcher);
    return {};
}

Result<void> Server::request_shutdown() {
    return main_->loop().post([this] { begin_shutdown(); });
}

void Server::stop() noexcept {
    main_->loop().stop();
}

Endpoint Server::local_endpoint() const {
    return listener_->local_endpoint();
}

// ---- accepting ----------------------------------------------------------------------------------

/// New descriptor from the acceptor (loop 0). Over the cap the client is closed at once (the Fd
/// destructor) without reading a byte: refusing costs one close (FR-S1, T4). Otherwise the
/// connection is counted and given to the next loop in turn. Runs inside the acceptor's callback,
/// which must not throw.
void Server::on_accept(Fd client, const Endpoint& peer) {
    if (live_.load() >= config_.max_connections) {
        const std::uint64_t total = rejected_.fetch_add(1) + 1;
        if ((total & (total - 1)) == 0) {  // log the 1st, 2nd, 4th, 8th...: rate-limited
            logger_.log(LogLevel::Warn, "server: connection limit reached, refusing clients");
        }
        return;
    }
    live_.fetch_add(1);  // only this thread adds, so the check above cannot be overtaken
    const ConnectionId id = ++last_id_;
    const std::size_t target = next_shard_++ % (1 + workers_.size());
    if (target == 0) {
        main_->adopt(std::move(client), peer, id);
    } else {
        hand_off(target - 1, std::move(client), peer, id);
    }
}

/// Give the descriptor to another loop. The task must be copyable, so the move-only Fd travels in
/// a shared_ptr; if the task is never run (the loop is gone) the Fd is closed with it.
void Server::hand_off(std::size_t worker, Fd client, const Endpoint& peer, ConnectionId id) {
    LoopShard& shard = workers_[worker]->shard();
    bool handed_over = false;
    try {
        auto holder = std::make_shared<Fd>(std::move(client));
        handed_over =
            shard.loop()
                .post([&shard, holder, peer, id] { shard.adopt(std::move(*holder), peer, id); })
                .has_value();
    } catch (const std::bad_alloc&) {
        handed_over = false;  // refused like a full queue
    }
    if (!handed_over) {
        live_.fetch_sub(1);
        rejected_.fetch_add(1);
        logger_.log(LogLevel::Warn, "server: a loop is overloaded, refusing a client");
    }
}

// ---- shutdown -----------------------------------------------------------------------------------

void Server::on_signal(int /*signo*/) {
    logger_.log(LogLevel::Info, "server: shutdown signal received");
    begin_shutdown();  // the second signal lands in force_shutdown()
}

/// EC-C18: stop accepting, send 1001 to everyone on every loop, give them `shutdown_grace`, then
/// drop the rest. Runs on loop 0, which tells the others by posting.
void Server::begin_shutdown() {
    if (shutting_down_) {
        force_shutdown();
        return;
    }
    shutting_down_ = true;
    acceptor_->stop();
    shards_to_drain_ = 1 + workers_.size();
    for (const std::unique_ptr<WorkerThread>& worker : workers_) {
        LoopShard& shard = worker->shard();
        if (!shard.loop().post([&shard] { shard.begin_shutdown(); })) {
            logger_.log(LogLevel::Error, "server: cannot tell a loop to shut down, forcing");
            force_shutdown();
            return;
        }
    }
    main_->begin_shutdown();  // may already be drained: then this ends loop 0 below
    const Result<TimerHandle> grace =
        main_->loop().run_after(config_.shutdown_grace, [this] { force_shutdown(); });
    if (grace) {
        grace_timer_ = *grace;
    } else {
        force_shutdown();  // no timer, no way to wait: do not hang
    }
}

/// Loop 0 drops its own connections and ends; run() then stops the other loops, whose shards
/// close what is left when they are destroyed.
void Server::force_shutdown() {
    main_->abort_all();
    main_->loop().stop();
}

/// A shard has no connection left (any loop thread). When all have none, the shutdown is done.
/// Only loop 0 is touched from here: the other shards may already be on their way out.
void Server::on_shard_drained() {
    if (drained_.fetch_add(1) + 1 == shards_to_drain_) {
        main_->loop().stop();
    }
}

}  // namespace wsrv
