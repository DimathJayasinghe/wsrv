// SPDX-License-Identifier: Apache-2.0
#include "server/server.hpp"

#include "base/assert.hpp"
#include "net/fd_budget.hpp"
#include "net/system_random.hpp"

#include <array>
#include <csignal>
#include <exception>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <sys/socket.h>

namespace wsrv {

Server::Server(Key /*unused*/, ServerConfig config, const ServerDeps& deps)
    : config_{std::move(config)}, factory_{*deps.factory}, decider_{deps.decider},
      publish_sources_{deps.publish}, wrapper_{deps.transport_wrapper},
      sys_{deps.sys != nullptr ? *deps.sys : static_cast<ISyscalls&>(own_sys_)},
      clock_{deps.clock != nullptr ? *deps.clock : static_cast<const IClock&>(own_clock_)},
      logger_{deps.logger != nullptr ? *deps.logger : static_cast<ILogger&>(own_logger_)} {}

Server::~Server() = default;

namespace {

/// 127.0.0.0/8 or ::1.
[[nodiscard]] bool is_loopback(const Endpoint& endpoint) noexcept {
    if (!endpoint.v6) {
        return endpoint.address[0] == 127;
    }
    for (std::size_t i = 0; i + 1 < endpoint.address.size(); ++i) {
        if (endpoint.address[i] != 0) {
            return false;
        }
    }
    return endpoint.address.back() == 1;
}

/// Descriptors the process needs besides one per connection (ADR-0022): the listener, the
/// acceptor's EMFILE reserve, the signal descriptor, stdio, log and certificate files, and the
/// embedding host's own. Generous on purpose: a too-small guess fails at start, a large one costs
/// nothing.
constexpr std::size_t kDescriptorReserve = 64;
/// Each loop holds an epoll instance and its wake-up eventfd (plus slack).
constexpr std::size_t kDescriptorsPerLoop = 4;

[[nodiscard]] std::uint64_t required_descriptors(const ServerConfig& config) noexcept {
    // Both factors are bounded by validate() (ceilings of 2^20 and 256), so this cannot overflow.
    return static_cast<std::uint64_t>(config.max_connections + kDescriptorReserve +
                                      kDescriptorsPerLoop * config.threads);
}

/// T4: a server that cannot hold its own cap would meet EMFILE under load. Say so now, with the
/// numbers an operator needs, before anything is bound (ADR-0022).
[[nodiscard]] Result<void>
reserve_descriptors(const ServerConfig& config, ISyscalls& sys, ILogger& logger) {
    const std::uint64_t needed = required_descriptors(config);
    const Result<FdBudget> budget = ensure_fd_budget(sys, needed);
    // Formatting the numbers allocates; out of memory at start is itself a reason not to start.
    try {
        if (!budget) {
            FdLimits limits;
            static_cast<void>(sys.get_fd_limits(limits));
            logger.log(LogLevel::Error,
                       "descriptor_limit",
                       "server: needs " + std::to_string(needed) +
                           " descriptors (max_connections " +
                           std::to_string(config.max_connections) +
                           " + reserve) but the limit is " + std::to_string(limits.soft) +
                           " soft / " + std::to_string(limits.hard) + " hard (RLIMIT_NOFILE)");
        } else if (budget->raised) {
            logger.log(LogLevel::Info,
                       "descriptor_limit_raised",
                       "server: raised the soft descriptor limit to " +
                           std::to_string(budget->soft));
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    if (!budget) {
        return std::unexpected{budget.error()};
    }
    return {};
}

}  // namespace

Result<std::unique_ptr<Server>> Server::create(ServerConfig config, const ServerDeps& deps) {
    if (deps.factory == nullptr) {
        return fail(ErrorCode::InvalidArgument, "server: a handler factory is required");
    }
    if (const Result<void> valid = validate(config); !valid) {
        return std::unexpected{valid.error()};
    }
    if (config.publish.has_value() && deps.publish == nullptr) {
        return fail(ErrorCode::InvalidConfig, "server: publish needs its token and action");
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

/// Everything the shards share, in dependency order (ADR-0014 section 2).
Result<void> Server::make_shared_state() {
    try {
        negotiator_ = std::make_unique<HandshakeNegotiator>(config_.handshake);
        pending_ = std::make_unique<PendingGate>(effective_max_pending_handshakes(config_));
        if (const Result<void> limits = make_ip_limits(); !limits) {
            return limits;
        }
        banks_ = std::make_unique<MetricBanks>(config_.threads);
        shared_ = std::make_unique<ShardShared>(ShardShared{config_,
                                                            *negotiator_,
                                                            factory_,
                                                            sys_,
                                                            clock_,
                                                            logger_,
                                                            live_,
                                                            decider_,
                                                            pending_.get(),
                                                            wrapper_,
                                                            ip_limits_.get(),
                                                            *banks_});
    } catch (const std::invalid_argument&) {
        return fail(ErrorCode::InvalidConfig, "server: invalid handshake configuration");
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    return {};
}

/// The fallible half of construction, in dependency order: negotiator, loop 0's shard, listener
/// (bind errors surface here, EC-N20), acceptor. The other loops start in run().
Result<void> Server::init() {
    if (const Result<void> descriptors = reserve_descriptors(config_, sys_, logger_);
        !descriptors) {
        return descriptors;
    }
    if (const Result<void> shared = make_shared_state(); !shared) {
        return shared;
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
        AcceptorConfig{.metrics = &main_->metrics()});
    if (const Result<void> started = acceptor_->start(); !started) {
        return started;
    }
    if (const Result<void> admin = start_admin(); !admin) {
        return admin;
    }
    return start_publish();
}

/// The publish listener, on loop 0 (ADR-0028 section 3), after the admin one. It counts into loop
/// 0's bank, so its answers reach /metrics like everything else.
Result<void> Server::start_publish() {
    if (!config_.publish) {
        return {};
    }
    PublishConfig publish;
    publish.listen = *config_.publish;
    publish.max_connections = config_.publish_max_connections;
    publish.request_timeout = config_.publish_request_timeout;
    publish.max_body_bytes = config_.publish_max_body_bytes;
    auto endpoint = PublishEndpoint::create(PublishDeps{main_->loop(),
                                                        sys_,
                                                        *publish_sources_,
                                                        main_->metrics(),
                                                        [this] { return !shutting_down_; },
                                                        &logger_},
                                            publish);
    if (!endpoint) {
        return std::unexpected{endpoint.error()};
    }
    publish_ = std::move(*endpoint);
    const Endpoint bound = publish_->local_endpoint();
    try {
        logger_.log(LogLevel::Info,
                    "publish_listening",
                    "server: publish listening on " + format_endpoint(bound));
        if (!is_loopback(bound)) {
            logger_.log(LogLevel::Warn,
                        "publish_exposed",
                        "server: the publish port is not on loopback; the token travels in clear "
                        "text unless a TLS proxy or a private network protects it");
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    return {};
}

/// The admin listener, on loop 0 (ADR-0023 section 3). Its pages read what already exists: the
/// counters, the live count and whether a shutdown has begun (a loop-0 flag, read on loop 0).
Result<void> Server::start_admin() {
    if (!config_.admin) {
        return {};
    }
    AdminConfig admin;
    admin.listen = *config_.admin;
    admin.max_connections = config_.admin_max_connections;
    admin.request_timeout = config_.admin_request_timeout;
    AdminSources sources;
    try {
        sources.metrics_text = [this] { return metrics_text(); };
        sources.ready = [this] { return !shutting_down_; };
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    auto endpoint = AdminEndpoint::create(main_->loop(), sys_, admin, std::move(sources));
    if (!endpoint) {
        return std::unexpected{endpoint.error()};
    }
    admin_ = std::move(*endpoint);
    const Endpoint bound = admin_->local_endpoint();
    try {
        logger_.log(LogLevel::Info,
                    "admin_listening",
                    "server: admin listening on " + format_endpoint(bound));
        if (!is_loopback(bound)) {
            logger_.log(LogLevel::Warn,
                        "admin_exposed",
                        "server: the admin port is not on loopback and has no authentication");
        }
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    return {};
}

/// The per-IP table (ADR-0021), unless both limits are off. Its hash is keyed from the OS random
/// source: source addresses are chosen by the peer, so an unkeyed hash could be made to collide.
/// No random source means no table, and no table would mean no limits, so that fails the start.
Result<void> Server::make_ip_limits() {
    if (config_.max_connections_per_ip == 0 && config_.max_new_connections_per_ip_per_second == 0) {
        return {};
    }
    std::array<std::byte, sizeof(std::uint64_t)> seed_bytes{};
    SystemRandom random;
    if (const Result<void> filled = random.fill(MutableByteView{seed_bytes}); !filled) {
        return fail(ErrorCode::RandomUnavailable, "server: no random source for the per-IP table");
    }
    IpLimitsConfig limits;
    limits.max_connections_per_ip = config_.max_connections_per_ip;
    limits.max_new_connections_per_second = config_.max_new_connections_per_ip_per_second;
    limits.max_tracked = 2 * config_.max_connections;  // each live connection holds one address
    for (const std::byte b : seed_bytes) {
        limits.hash_seed = (limits.hash_seed << 8U) | std::to_integer<std::uint64_t>(b);
    }
    try {
        ip_limits_ = std::make_unique<IpLimits>(limits, clock_);
    } catch (const std::bad_alloc&) {
        return fail(ErrorCode::ResourceExhausted, "server: out of memory");
    }
    return {};
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
        main_->metrics().add(Metric::RejectedGlobalCap);
        const std::uint64_t total = main_->metrics().read(Metric::RejectedGlobalCap);
        if ((total & (total - 1)) == 0) {  // log the 1st, 2nd, 4th, 8th...: rate-limited
            logger_.log(
                LogLevel::Warn, "conn_limit", "server: connection limit reached, refusing clients");
        }
        return;
    }
    // A trusted proxy (ADR-0029) takes no per-IP slot here: its clients are admitted by the
    // connection once the upgrade request names them. The shard decides the same way on release.
    if (ip_limits_ != nullptr && !is_trusted_proxy(config_.trusted_proxies, peer)) {
        if (const Admission verdict = ip_limits_->admit(peer); verdict != Admission::Admitted) {
            refuse_for_ip_limit(std::move(client), verdict);
            return;
        }
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

namespace {
// Prebuilt answers for a refused address: one non-blocking send on a fresh socket (its send buffer
// is empty, so these few bytes always fit), no allocation, no connection object (ADR-0021).
constexpr std::string_view kAnswer429 = "HTTP/1.1 429 Too Many Requests\r\nRetry-After: "
                                        "1\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
constexpr std::string_view kAnswer503 = "HTTP/1.1 503 Service Unavailable\r\nRetry-After: "
                                        "1\r\nConnection: close\r\nContent-Length: 0\r\n\r\n";
}  // namespace

/// An address over its limit: tell it so in one send and close. Best effort by design: a client
/// that already sent its request may see a reset instead; the close is what counts. The table
/// being full (an address-spraying flood) is a 503, the limits themselves are a 429.
void Server::refuse_for_ip_limit(Fd client, Admission verdict) {
    const Metric reason = refusal_metric(verdict);
    main_->metrics().add(reason);
    const MetricsSnapshot now = banks_->snapshot();
    const std::uint64_t total = now.value(Metric::RejectedIpConnections) +
                                now.value(Metric::RejectedIpRate) +
                                now.value(Metric::RejectedIpTable);
    if ((total & (total - 1)) == 0) {  // log the 1st, 2nd, 4th, 8th...: rate-limited
        logger_.log(LogLevel::Warn, "ip_limit", "server: per-IP limit reached, refusing clients");
    }
    const std::string_view answer = verdict == Admission::TableFull ? kAnswer503 : kAnswer429;
    static_cast<void>(
        sys_.send(client.get(), answer.data(), answer.size(), MSG_NOSIGNAL | MSG_DONTWAIT));
}  // the descriptor is closed here

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
        if (ip_limits_ != nullptr) {
            ip_limits_->release(peer);
        }
        main_->metrics().add(Metric::RejectedGlobalCap);  // shed like a full table: no loop took it
        logger_.log(
            LogLevel::Warn, "loop_overloaded", "server: a loop is overloaded, refusing a client");
    }
}

// ---- shutdown -----------------------------------------------------------------------------------

void Server::on_signal(int /*signo*/) {
    logger_.log(LogLevel::Info, "shutdown", "server: shutdown signal received");
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
            logger_.log(LogLevel::Error,
                        "shutdown_failed",
                        "server: cannot tell a loop to shut down, forcing");
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
