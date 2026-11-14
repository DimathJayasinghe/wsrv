// SPDX-License-Identifier: Apache-2.0
#pragma once

#include "base/clock.hpp"
#include "base/error.hpp"
#include "base/logger.hpp"
#include "base/metrics.hpp"
#include "net/acceptor.hpp"
#include "net/endpoint.hpp"
#include "net/event_loop.hpp"
#include "net/signal_watcher.hpp"
#include "net/syscalls.hpp"
#include "net/tcp_listener.hpp"
#include "net/transport_wrapper.hpp"
#include "protocol/handshake.hpp"
#include "server/admin_endpoint.hpp"
#include "server/config.hpp"
#include "server/handler.hpp"
#include "server/handshake_decider.hpp"
#include "server/ip_limits.hpp"
#include "server/loop_shard.hpp"
#include "server/pending_gate.hpp"
#include "server/worker_thread.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace wsrv {

/// Collaborators injected at the composition root. Null means "use the real one" (Linux system
/// calls, the steady clock, no logging). `factory` is required.
/// With `threads > 1` the factory and the logger are called from several loop threads at once and
/// must be thread-safe (ADR-0014).
struct ServerDeps {
    IHandlerFactory* factory = nullptr;
    ISyscalls* sys = nullptr;
    const IClock* clock = nullptr;
    ILogger* logger = nullptr;
    /// Opt-in asynchronous handshake decision (ADR-0018). Null = valid upgrades are accepted at
    /// once. Borrowed; must be thread-safe (it is asked from every loop thread).
    IHandshakeDecider* decider = nullptr;
    /// Wraps each accepted transport (TLS, ADR-0019). Null = connections are plain TCP. Borrowed;
    /// must be thread-safe (it is called from every loop thread).
    ITransportWrapper* transport_wrapper = nullptr;
};

/// A WebSocket server (Facade): accepts connections, runs them on one or more loops, shuts them
/// down. `ServerConfig::threads` loops run; loop 0 runs on the thread that calls run(), owns the
/// listener and also serves connections, the others run on threads the server starts inside run()
/// and stops before it returns. The acceptor hands each new connection to the next loop in turn
/// (ADR-0014). With one thread nothing is handed over.
///
/// Lifecycle: create() on the thread that will call run() (loop 0 belongs to that thread),
/// optionally install_signal_handlers(), then run(), which returns after a shutdown or stop().
/// request_shutdown() and stop() may be called from any thread while run() is active:
///  - request_shutdown(): graceful (FR-S5, EC-C18). Stop accepting, send Close 1001 to every
///    client, wait up to `shutdown_grace`, then drop the rest. A second request means "now".
///  - stop(): end run() at once, without closing handshakes.
/// Destroy the Server on the loop-0 thread, after run() has returned (the loop checks its thread).
///
/// Thread-affinity: create/run/install_signal_handlers/destructor on the loop-0 thread;
/// request_shutdown and stop are any-thread.
class Server {
public:
    /// @param config must have passed validate(); the HandshakeConfig is validated here.
    /// @return Error{InvalidConfig} (bad config), Error{AddressInUse} / Error{Io} (bind failed,
    /// EC-N20), Error{InvalidArgument} (no handler factory).
    [[nodiscard]] static Result<std::unique_ptr<Server>> create(ServerConfig config,
                                                                const ServerDeps& deps);

    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;
    Server(Server&&) = delete;
    Server& operator=(Server&&) = delete;
    ~Server();

    /// Start the other loops, run loop 0 until stop() or the end of a shutdown, then stop and
    /// join the other loops. @return the first loop's fatal error, or the error that kept a loop
    /// thread from starting. Call at most once.
    [[nodiscard]] Result<void> run();

    /// Turn SIGINT and SIGTERM into a graceful shutdown (a second one forces it). Blocks the
    /// signals for the calling thread: call it on the loop-0 thread before run(), so the loop
    /// threads started by run() inherit the mask.
    [[nodiscard]] Result<void> install_signal_handlers();

    [[nodiscard]] Result<void> request_shutdown();
    void stop() noexcept;

    /// The address actually bound (the real port when 0 was configured).
    [[nodiscard]] Endpoint local_endpoint() const;
    /// The admin listener's bound address (ADR-0023); empty when the admin listener is off.
    [[nodiscard]] std::optional<Endpoint> admin_endpoint() const {
        return admin_ ? std::optional<Endpoint>{admin_->local_endpoint()} : std::nullopt;
    }
    /// Connections refused because max_connections was reached. Any-thread.
    [[nodiscard]] std::uint64_t rejected_connections() const noexcept {
        return metrics().value(Metric::RejectedGlobalCap);
    }
    /// Connections refused by a per-IP limit (ADR-0021), not counted in rejected_connections().
    /// Any-thread.
    [[nodiscard]] std::uint64_t ip_limited_connections() const noexcept {
        const MetricsSnapshot now = metrics();
        return now.value(Metric::RejectedIpConnections) + now.value(Metric::RejectedIpRate) +
               now.value(Metric::RejectedIpTable);
    }
    /// The counters of every loop summed (ADR-0023): a snapshot taken counter by counter.
    /// Any-thread.
    /// A counter bank any thread may add to (the broker's, ADR-0024 section 6). Its counts appear
    /// in metrics() like every other bank's. Reference counted: a holder that outlives the server
    /// keeps writing to valid memory that nobody reads any more (ADR-0024 A3).
    [[nodiscard]] std::shared_ptr<IMetrics> shared_metrics() const noexcept {
        return banks_->shared();
    }
    [[nodiscard]] MetricsSnapshot metrics() const noexcept {
        return banks_->snapshot();
    }
    /// The values that are state, not events. Any-thread.
    [[nodiscard]] MetricsGauges metrics_gauges() const noexcept {
        return MetricsGauges{live_.load()};
    }
    /// Prometheus text of metrics() and metrics_gauges(), what the admin endpoint serves.
    [[nodiscard]] std::string metrics_text() const {
        return render_prometheus(metrics(), metrics_gauges());
    }

    /// Construction goes through create() (passkey, so make_unique can call it).
    class Key {
        friend class Server;
        Key() = default;
    };
    Server(Key /*unused*/, ServerConfig config, const ServerDeps& deps);

private:
    [[nodiscard]] Result<void> init();
    [[nodiscard]] Result<void> make_ip_limits();
    [[nodiscard]] Result<void> start_workers();
    void stop_workers();
    void on_accept(Fd client, const Endpoint& peer);
    [[nodiscard]] static Metric refusal_metric(Admission verdict) noexcept;
    void refuse_for_ip_limit(Fd client, Admission verdict);
    void hand_off(std::size_t worker, Fd client, const Endpoint& peer, ConnectionId id);
    void begin_shutdown();
    void force_shutdown();
    [[nodiscard]] Result<void> start_admin();
    [[nodiscard]] Result<void> make_shared_state();
    void on_shard_drained();
    void on_signal(int signo);

    ServerConfig config_;
    IHandlerFactory& factory_;
    IHandshakeDecider* decider_;  // null = the hook is off
    ITransportWrapper* wrapper_;  // null = plain TCP
    LinuxSyscalls own_sys_;
    SteadyClock own_clock_;
    NullLogger own_logger_;
    ISyscalls& sys_;
    const IClock& clock_;
    ILogger& logger_;

    // Non-null once init() succeeded (create() returns only then).
    // Destruction runs in reverse declaration order. The acceptor and the signal watcher come
    // last: they are destroyed first and stop feeding new work. Then the worker threads (their
    // shards die on their own threads), then loop 0's shard, which everything above may use.
    std::atomic<std::size_t> live_{0};      // connections alive in every shard: the global cap
    std::unique_ptr<PendingGate> pending_;  // upgrades waiting for the decider; outlives the shards
    std::unique_ptr<IpLimits> ip_limits_;   // null = no per-IP limits; outlives the shards
    std::unique_ptr<MetricBanks> banks_;    // one counter bank per loop; outlives the shards
    std::unique_ptr<HandshakeNegotiator> negotiator_;
    std::unique_ptr<ShardShared> shared_;
    std::unique_ptr<LoopShard> main_;  // shard 0, on the thread that created the server
    std::vector<std::unique_ptr<WorkerThread>> workers_;  // shards 1..threads-1, started by run()
    std::unique_ptr<TcpListener> listener_;
    std::unique_ptr<Acceptor> acceptor_;
    std::unique_ptr<AdminEndpoint> admin_;  // on loop 0; null = off; destroyed before loop 0
    std::unique_ptr<SignalWatcher> signals_;

    // Loop-0-thread state.
    ConnectionId last_id_ = 0;
    std::size_t next_shard_ = 0;
    bool shutting_down_ = false;
    std::size_t shards_to_drain_ = 0;  // set by begin_shutdown() before any shard is told
    std::optional<TimerHandle> grace_timer_;

    std::atomic<std::size_t> drained_{0};
};

}  // namespace wsrv
